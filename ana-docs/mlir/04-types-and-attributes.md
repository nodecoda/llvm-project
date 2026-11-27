# 04 类型与属性解析

类型和属性是 MLIR 语法的“表达式层”：它们递归嵌套、可带类型标注、可引用别名。
本文对应 `TypeParser.cpp`（634 行）与 `AttributeParser.cpp`（1404 行），
两者互相递归：属性的 `: type` 要解析类型，类型的 memref layout 要解析属性。

## 1. 类型解析

### 1.1 入口与分派

```cpp
// TypeParser.cpp:77
Type Parser::parseType() {
  if (getToken().is(Token::l_paren))
    return parseFunctionType();      // "(a, b) -> c"
  return parseNonFunctionType();     // 其余全部
}
```

`parseOptionalType`（`:31`）用一份“合法类型起始 token”白名单（`:34-74`）判断
“当前位置是否可能开始一个类型”，不匹配时返回 `std::nullopt` 而不报错。
这份白名单就是“类型语法起点”的权威清单，新增内置类型必须同步维护。

`parseNonFunctionType`（`:278`）是 `Token::Kind` 的大 `switch`：

| token | 类型 | 备注 |
| --- | --- | --- |
| `kw_memref` | `parseMemRefType`（`:186`） | ranked/unranked |
| `kw_tensor` | `parseTensorType`（`:406`） | 可选 encoding |
| `kw_complex` | `parseComplexType`（`:137`） | 元素限 float/integer |
| `kw_tuple` | `parseTupleType`（`:464`） | 允许空 `tuple<>` |
| `kw_vector` | `parseVectorType`（`:490`） | scalable 维度 |
| `inttype` | `IntegerType` | `i8`/`si8`/`ui8`，宽度上限 `kMaxWidth` |
| `kw_index` / float 关键字 / `kw_none` / `kw_token` | 对应单例类型 | |
| `exclamation_identifier` | `parseExtendedType()` | 交给 dialect |
| 其它 | `expected non-function type` | |

### 1.2 function type

```ebnf
function-type        ::= type-list-parens "->" function-result-type
function-result-type ::= type-list-parens | type
type-list-parens     ::= "(" (type ("," type)*)? ")"
```

`parseFunctionType`（`:160`）先解析参数列表，再解析结果（单个类型或括号列表），
最后 `builder.getFunctionType`。注意：**只有顶层 `(` 才进入函数类型分支**，
因此 `(i32, i64)` 作为“类型列表”与函数类型的歧义靠上下文（调用方）消除。

### 1.3 memref / tensor

```ebnf
memref-type  ::= "memref" "<" ("*x" | dimension-list) type
                 ("," (layout | memory-space))* ">"
tensor-type  ::= "tensor" "<" ("*x" | dimension-list) type
                 ("," encoding)? ">"
dimension-list ::= dimension ("x" dimension)*
dimension      ::= "?" | integer
```

- `memref<...>` 的逗号项按“是不是 `MemRefLayoutAttrInterface`”区分 layout 与
  memory space，并强制 memory space 只能出现一个且必须在最后（`TypeParser.cpp:236-257`）。
- unranked（`*x`）不允许 affine map layout。
- tensor 的 encoding 必须通过 `VerifiableTensorEncoding` 校验（`:437-444`）。
- 元素类型合法性：memref 用 `BaseMemRefType::isValidElementType`（`:221`），
  tensor 用 `TensorType::isValidElementType`（`:452`）。

### 1.4 vector 与维度列表的“粘连”

```ebnf
vector-type ::= "vector" "<" ("[" integer "]")? ("x" dimension)* "x" type ">"
```

`2x3xf32` 在词法层会切成 `integer(2)`、`bare_identifier(x3xf32)`——因为裸标识符
允许字母数字混排。为处理这种情况，类型解析器直接操作 lexer 指针：

- `parseIntegerInDimensionList`（`:594`）：若当前是 `0x...`，只取 `0`，
  然后 `state.lex.resetPointer(spelling.data() + 1)` 让下一个 token 从 `x` 开始；
- `parseXInDimensionList`（`:622`）：若当前裸标识符以 `x` 开头（如 `x3xf32`），
  重置指针到 `x` 之后，把 `x` 当作维度分隔符消费。

这是整套解析器里少数“回退 lexer 指针”的地方，也是最容易读错的实现细节。

## 2. 属性解析

### 2.1 入口

`Parser::parseAttribute(Type type)`（`AttributeParser.cpp:49`）按首 token 分派：

```ebnf
attribute ::= affine-map-attr | affine-set-attr
            | "array" "<" type ":" elems ">"
            | "true" | "false"
            | "dense" "<" (type ":" elems | tensor-literal) ">" ":" shaped-type
            | "dense_resource" "<" handle ">" (":" shaped-type)?
            | attr-dict
            | extended-attribute            // "#..."
            | float-literal | integer | "-" (integer | float)
            | "loc" "(" location ")"
            | "sparse" "<" indices "," values ">" ":" shaped-type
            | "strided" "<" "[" strides "]" ("," "offset" ":" integer)? ">"
            | string (":" type)?
            | symbol-ref                    // "@..."
            | "unit"
            | "distinct" "[" integer "]" "<" attribute ">"
            | type                          // 类型当作 TypeAttr
```

`type` 参数是“期望类型”：来自 `parseAttribute` 的调用方（如自定义 assembly 里
`parser.parseAttribute(attr, type)`），用于给无标注字面量补语义、并做类型一致性检查。

### 2.2 “类型即属性”规则

`parseOptionalAttribute`（`:245`）在 token 不在白名单时，会尝试 `parseOptionalType`
并包装成 `TypeAttr`：

- 这解释了 `{my_flag = i32}`、`{align = 64 : i64}` 里的 `i32`；
- 也解释了为什么 `parseAttributeDict` 可以直接复用 `parseAttribute`，
  而不需要单独的类型分支。

### 2.3 数字字面量的默认类型

| 输入 | 无类型标注时 | 代码 |
| --- | --- | --- |
| 浮点 `1.5` | `f64` | `parseFloatAttr`（`:341`） |
| 整数 `42` | `i64` | `parseDecOrHexAttr`（`:413`） |
| 带标注 `42 : i8` | 校验宽度/符号 | `buildAttributeAPInt`（`:367`） |

`buildAttributeAPInt` 会按目标位宽 zext/trunc，并用 `countl_zero` 检查是否丢失有效位；
负数对 unsigned 类型直接报错，正数对 signed/index 要求符号位为 0（`:367-411`）。

浮点：`parseFloatAttr` 先只记 token，等拿到类型（含 float semantics）后再用
`APFloat::convertFromString` 按目标语义解析，避免经 `double` 损失 f80 等宽格式的精度。

### 2.4 attr-dict

```ebnf
attr-dict     ::= "{" "}" | "{" entry ("," entry)* "}"
entry         ::= key ("=" attribute)?
key           ::= bare-id | string | inttype | keyword
```

`parseAttributeDict`（`:296`）：

- `SmallDenseSet<StringAttr> seenKeys` 做重复键检测（`:311-313`）；
- 没有 `=` 的键视为 `unit` 属性（`:318-322`），例如 `{llvm.emit_c_interface}`；
- 键含 `.` 时按方言前缀预加载 dialect（`:316-318`），以便后续自定义属性解析。

### 2.5 dense：两种语法

`dense` 是 MLIR 里最复杂的属性，有两套写法：

```mlir
// type-first：显式元素类型
dense<[1, 2, 3] : vector<3xi32>>
// literal-first：从元素推断类型（后面必须再给 shaped type）
dense<[[1, 2], [3, 4]]> : tensor<2x2xi32>
// 十六进制原始数据
dense<"0x00000001"> : tensor<1xi32>
```

实现顺序（`:1098-1125`）：

1. 先试 `parseDenseElementsAttrTyped`（`:992`）：若 `<` 后是 `(`，这一定是
   嵌套列表/复数元素，直接放弃 typed 分支（`:993-1000` 注释）；
2. 否则回退 `TensorLiteralParser`（`:472`）+ `parseElementsLiteralType`：
   - `parseList`（`:818`）递归降维，`checkDims` 要求兄弟列表 rank 一致；
   - `getAttr`（`:548`）按元素类型构造 int/float/complex/string/hex；
   - 最终 shape 必须与 `: tensor<...>` 完全一致（`:558-563`）。

十六进制 blob 走 `getHexAttr`（`:730`）：先 `DenseElementsAttr::isValidRawBuffer`
校验大小，大端机上再做端序翻转（`mxi8` 单字节除外）。

### 2.6 其它内置属性

| 属性 | 语法 | 关键点 |
| --- | --- | --- |
| `array` | `array<i32: 1, 2>` | 原始字节累积；元素须为 bool/int/float，位宽为 8 的倍数（`i1` 除外） |
| `sparse` | `sparse<[[0],[1]], [1.0, 2.0]> : tensor<2xf32>` | indices 为 i64 的 2D 形状（N × rank），支持 splat，values 形状由 indices 推出 |
| `strided` | `strided<[1, 2], offset: 5>` | 调 `StridedLayoutAttr::verify` |
| `distinct` | `distinct[7]<42>` | 同一 ID 必须引用同一属性，否则报错（`:1389-1400`） |
| symbol-ref | `@a::@b::@c` | `::` 逐级嵌套，`parseAttribute` 用双冒号前瞻 |
| `unit` | `unit` | `UnitAttr` |

`distinct` 的去重表是 `state.symbols.distinctAttributes`（`ParserState.h:42`），
**跨整个解析会话**生效：同一 ID 出现两次但引用属性不同会直接报错。

## 3. 示例合集

```mlir
// 类型
tensor<2x?xf32>
memref<4xi8, strided<[1], offset: 0>>
vector<[4]x2xf32>              // 第 0 维 scalable
tuple<>
!llvm.ptr
!my.type<"payload">

// 属性
42 : i8
1.5                            // f64
dense<[[1, 2], [3, 4]]> : tensor<2x2xi32>
dense<"0x0000000100000002"> : tensor<2xi32>
array<i32: 1, 2, 3>
strided<[1, 2], offset: 5>
distinct[7]<42>
{foo = 1 : i32, bar, baz = i32}
@symbol::@nested
```

## 4. 常见错误与陷阱

1. **默认类型是隐藏语义**：`1.5` 是 f64、`42` 是 i64；写 `dense<[1, 2]> : tensor<2xi8>`
   会因元素宽度不符报错，需要显式 `1 : i8` 或使用 type-first 形式。
2. **`dense` 的判定顺序**：type-first 优先；以 `(` 开头（复数/嵌套列表）会跳过 typed 分支。
3. **`i0`/`si0`**：词法上仍是 `inttype`，宽度校验在类型解析阶段。
4. **`0x` 与维度 `x` 的歧义**：`0xf32` 里的 `0` 是维度、`x` 是分隔符，
   靠 `parseIntegerInDimensionList` 的指针回退解决。
5. **memref 的逗号项顺序**：memory space 必须在最后且只能一个；unranked 不能带 layout。
6. **`distinct` ID 泄漏**：同一文件里同一 ID 只能对应一个 referenced attribute。
7. **alias 与 dialect 属性**：`#map`（无 `.`）是 alias，`#gpu.thread<x>`（有 `.` 或跟 `<`）
   是 dialect 属性；见 05 章。

下一章：[05-dialect-and-interfaces.md](05-dialect-and-interfaces.md)。
