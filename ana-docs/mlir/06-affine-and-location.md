# 06 仿射与位置

`AffineParser.cpp`（821 行）和 `LocationParser.cpp`（237 行）是两个“半独立”的子语法：
它们复用 `Parser` 的 token 原语，但各自有自己的递归下降逻辑。

## 1. 仿射表达式/映射/整数集

### 1.1 语法

```ebnf
affine-map   ::= dim-list symbol-list? "->" multi-dim-expr
dim-list     ::= "(" (id ("," id)*)? ")"
symbol-list  ::= "[" (id ("," id)*)? "]"
multi-dim-expr ::= "(" (affine-expr ("," affine-expr)*)? ")"

affine-expr  ::= affine-expr ("+" | "-") affine-expr
               | affine-expr ("*" | "floordiv" | "ceildiv" | "mod") affine-expr
               | "(" affine-expr ")"
               | "-" affine-expr
               | bare-id | integer | "%" ssa-id
               | "symbol" "(" "%" ssa-id ")"

integer-set  ::= dim-list symbol-list? ":" "(" constraint ("," constraint)* ")"
constraint   ::= affine-expr (">=" | "<=" | "==") affine-expr
```

示例：

```mlir
affine_map<(d0, d1)[s0] -> (d0 + s0 * 2, d1 floordiv 4)>
affine_set<(d0) : (d0 - 1 >= 0, d0 - 10 <= 0)>
affine_map<(d0) -> ()>            // 零结果 map
affine_set<(d0) : ()>             // 空约束 = 永远为真（0 == 0）
```

### 1.2 实现结构

`AffineParser : public Parser`（`AffineParser.cpp:59`）持有一张“名字 → AffineExpr”
声明表 `dimsAndSymbols`（`:115-117`），以及可选的 SSA 回调（用于
`affine_map` 里直接写 `%v` 的扩展语法）。

**只有两级优先级**：

| 优先级 | 运算符 | 函数 |
| --- | --- | --- |
| 低 | `+` `-` | `parseAffineLowPrecOpExpr`（`:436`） |
| 高 | `*` `floordiv` `ceildiv` `mod` | `parseAffineHighPrecOpExpr`（`:226`） |

左结合、递归下降：低优先级循环里遇到高优先级运算符就下钻一层
（`:419-435` 的注释说明了这个设计）。一元 `-` 绑定强于二元、弱于括号，
所以 `parseNegateExpression` 只调用 operand 级解析（`:273-287`）。

**仿射合法性检查**在构造表达式时完成（`getAffineBinaryOpExpr`，`:126-164`）：

- `*`：至少一侧必须是符号或常量（两个 dim 相乘非法）；
- `floordiv`/`ceildiv`/`mod`：右侧必须是符号或常量。

这就是 `d0 * d1` 报 “non-affine expression” 的唯一出处。

### 1.3 两种“标识符来源”

1. **声明式**：`affine_map<(d0, d1)[s0] -> ...` 先把 `d0/d1/s0` 登记进
   `dimsAndSymbols`（`parseIdentifierDefinition`，`:499`），表达式里按名字查找；
   未声明报 `use of undeclared identifier`（`:311`）。
2. **SSA 式**：`%v` 首次出现时按 `(name, number)` 分配 dim/symbol 序号并回调
   `addOperand`（`parseSSAIdExpr`，`:315`）；再次出现复用同一 `AffineExpr`。
   `symbol(%v)` 强制按 symbol 处理（`:338`）。
   这一模式由 `Parser::parseAffineMapOfSSAIds`（`:760`）驱动，供
   `OpAsmParser::parseAffineMapOfSSAIds` 使用（例如 `linalg` 的 indexing maps）。

**顺序约束**：SSA 式 map 必须“所有 dim 在前、symbol 在后”，
`Parser.cpp:1849-1851` 在汇总操作数时先 dim 后 symbol，否则 `AffineMap::get`
的 `numDims` 会错。

### 1.4 常量

`parseIntegerExpr`（`:353`）接受十进制整数，并特意允许 `9223372036854775808`
（2^63），用于表示 `INT64_MIN` 的幅值；超出报 `constant too large for index`。

### 1.5 约束归一化

`parseAffineConstraint`（`:648`）把二元比较统一成“表达式 ≥ 0 / == 0”：

| 输入 | 归一化 |
| --- | --- |
| `lhs >= rhs` | `lhs - rhs >= 0` |
| `rhs >= lhs` | `rhs - lhs >= 0`（即 `lhs - rhs <= 0`） |
| `lhs <= rhs` | `rhs - lhs >= 0` |
| `lhs == rhs` | `lhs - rhs == 0`（`isEq = true`） |

空约束列表被特判为 `0 == 0`（`:713-721`），而不是“无约束”的空集合。

### 1.6 公共 API

| API | 位置 | 说明 |
| --- | --- | --- |
| `Parser::parseAffineMapOrIntegerSetReference` | `AffineParser.cpp:731` | `affine_map<...>` / `affine_set<...>` 的入口 |
| `Parser::parseAffineMapReference` / `parseIntegerSetReference` | `:735` / `:748` | 带类型断言（解析到错的种类会报错） |
| `Parser::parseAffineExprReference` | `:744` | 预置 symbol 集合的表达式 |
| `mlir::parseAffineMap` / `parseIntegerSet` | `:805` / `:814` | 独立字符串解析 |

## 2. Location

### 2.1 语法

```ebnf
location ::= "unknown"
           | "callsite" "(" location "at" location ")"
           | "fused" ("<" attribute ">")? "[" (location ("," location)*)? "]"
           | string (":" integer (":" integer ("to" integer? ":" integer)?)?)?
           | string "(" location ")"
           | "#" alias
```

示例：

```mlir
loc(unknown)
loc("foo.mlir":3:7)                 // FileLineColLoc
loc("foo.mlir":3)                   // FileLineColRange（无列）
loc("foo.mlir":3:7 to 4:2)          // FileLineColRange
loc("inline"("foo.mlir":3:7))       // NameLoc(name, child)
loc(callsite("a":1:2 at "b":3:4))   // CallSiteLoc
loc(fused<"meta">["a":1:2, unknown])// FusedLoc + metadata attribute
loc(#loc42)                         // 属性别名
#loc42 = loc("x":1:1)
```

### 2.2 关键实现点（`LocationParser.cpp`）

- `parseNameOrFileLineColRange`（`:101`）区分四种字符串开头形态：

  | 写法 | 结果类型 |
  | --- | --- |
  | `"f":line` | `FileLineColRange` |
  | `"f":line:col` | `FileLineColLoc` |
  | `"f":line:col to ...` | `FileLineColRange` |
  | `"name"` / `"name"(child)` | `NameLoc`（可带子 location） |

  判断 `to` 用 `isCurrentTokenAKeyword() && spelling == "to"`（`:138-141`），
  因为 `to` 是关键字 token，但这里按拼写比较而非 `kw_to`。

- `callsite` 里的 `at` 是裸标识符字符串比较，不是关键字（`:52-56`）。
- `fused` 的 metadata 是任意属性（`:72-83`），也就是说 location 语法可以嵌套
  完整属性语法；随后用 `parseCommaSeparatedList(Delimiter::Square, ...)`
  解析 location 列表（`:85-95`）。

### 2.3 location alias 的延迟解析

location alias 复用**属性别名表**：`#loc42` 就是 `#` 属性别名，只是值必须是
`LocationAttr`（`parseLocationInstance` 里 `parseExtendedAttr(Type())` 后
`dyn_cast<LocationAttr>`，`:203-211`）。

允许前向引用：

1. `parseLocationAlias`（`Parser.cpp:2237`）在别名尚未定义时，创建
   `OpaqueLoc` 哨兵（携带 `DeferredLocInfo` 下标）并记录 `{loc, identifier}`
   （`:2249-2260`）；
2. 文件尾 `OperationParser::finalize` 遍历整棵 IR，把带该 TypeID 的 `OpaqueLoc`
   替换成真正的 location（`:944-975`）；
3. 未定义报 `operation location alias was never defined`（`:955-957`），
   别名值不是 location 报 `expected location, but found '...'`（`:959-963`）。

对比：**类型别名不支持前向引用**——`parseExtendedSymbol` 查不到就直接报
`undefined symbol alias id`（`DialectSymbolParser.cpp:200-208`）。
这是两种别名在语义上的真实差异，文档/实现都不要假设一致。

### 2.4 `loc` 前缀的两种入口

| 场景 | 处理 |
| --- | --- |
| op 尾部 `... loc(...)` | `parseTrailingLocationSpecifier`（`Parser.cpp:2264`），支持 alias 与直接 location |
| 自定义格式里主动解析 | `OpAsmParser::parseOptionalLocationSpecifier`（`Parser.cpp:2019-2050`，`AsmParserImpl` 转发） |

两者都先判断 `#ident` **不含 `.`** 才是 alias；含 `.` 是 dialect 属性
（`Parser.cpp:2033-2037`、`:2278-2281`）。

## 3. 小结

- 仿射子语法是“两级优先级 + 合法性即时检查”，没有通用表达式框架；
- SSA 式 affine 依赖回调，且必须 dim 先于 symbol；
- location 的语法细节集中在“`:` 后是行还是行列”和“alias 允许前向引用”；
- 两种别名（type/attr 与 location）的解析策略不同，是最容易写错的边界。

下一章：[07-tooling-and-diagnostics.md](07-tooling-and-diagnostics.md)。
