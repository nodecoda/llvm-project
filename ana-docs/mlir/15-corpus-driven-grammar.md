# 15. 语料优先：从方言源程序反推文法

> 配套文件：
> [`corpus/`](corpus/)（6 个 `.dialect`，650 行）、
> [`dialect-grammar.y`](dialect-grammar.y)（47 条规则 / 98 条产生式，bison
> `-Wall -Wcounterexamples -Werror` **0 冲突**）、
> [`tools/dialect-corpus.py`](tools/dialect-corpus.py)（统计与审计）、
> [`tools/dialect-lex.c`](tools/dialect-lex.c)（参考词法器）。
>
> 与 14 章的关系：14 章回答**描述什么**（结构/文本/语义三投影），本章回答
> **怎么写**；但写法不是设计的，是从按 14 章模型写出的语料里长出来的。

## 0. 方法与纪律

前两版都栽在同一个地方：

| 版本 | 做法 | 失败原因 |
| --- | --- | --- |
| v1 `design-first-v1/` | 先写文法，再写 5 个样例 | 样例被文法裁剪：都能解析，但没有回答"这个方言在领域里是什么" |
| v2 `sketch/arith.dialect` | 先写 1 个样例，再谈文法 | 只有一个方言，看不出哪些构造是共性、哪些是 arith 特有 |

本章换成**语料优先**：把 arith / func / scf / cf 当成源程序写出来（外加
complex / pdl 补上属性与类型定义），再统计、再归纳。三条纪律：

1. **每条产生式至少有一个语料见证。** 没有见证的构造不写进文法——哪怕它
   "看起来应该有"。
2. **一个概念一种写法。** 语料里出现两种写法，说明模型没想清楚，先统一再归纳。
3. **归纳靠人，核对靠机器。** 文法是人写的；"是否 0 冲突"、"6 个文件是否
   全部 parse OK"、"每条产生式是否有见证"由工具回答。

## 1. 语料

| 文件 | 行数 | dialect | constraint | enum | schema | op | attr_def | type_def |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `arith.dialect` | 401 | 1 | 4 | 5 | 13 | 56 | | |
| `func.dialect` | 48 | 1 | | | | 5 | | |
| `scf.dialect` | 125 | 1 | 1 | | | 12 | | |
| `cf.dialect` | 44 | 1 | | | | 4 | | |
| `complex.dialect` | 10 | 1 | | | | | 1 | |
| `pdl.dialect` | 22 | 1 | | | | | | 4 |
| **合计** | **650** | **6** | **5** | **5** | **13** | **77** | **1** | **4** |

为什么是这六个：

- **arith** 提供 schema/refinement 的主语料：56 个 op，45 个来自 13 个 schema，
  11 个独立定义；含依赖类型约束（`type_of(lhs)`）、可选属性、位枚举与枚举组。
- **func** 提供 region、可变元数、`Assembly Custom`、父约束
  （`has_parent(func)`）。
- **scf** 提供多 region、可选 region（`optional<region>`）、点号 op 名
  （`reduce.return`、`forall.in_parallel`）、以及 `op if`/`op for` 这种
  与关键字同形的名字。
- **cf** 提供 successor（单个、多个、可变）、嵌套可变元数
  （`variadic<variadic<any>>`）。
- **complex / pdl** 提供 `attr_def` 与 `type_def`；pdl 还给出 `type_def type`
  这个"名字与类型原子同形"的极端例子。

没有覆盖的：接口**定义**（四个方言只 `Implements`，不定义）、
partial custom（`custom<...>` 内嵌在格式里）、`oilist`、region 的精确块数、
除 property 之外的 invariant。这些不写进文法（§8）。

## 2. 词法：语料里只有三类关键字

| 类别 | 词 | 出处 |
| --- | --- | --- |
| 声明（小写） | `dialect` `op` `schema` `constraint` `enum` `attr_def` `type_def` `from` `extends` `bits` `optional` `variadic` | 12 个 |
| 子句（首字母大写） | `Doc` `Operands` `Results` `Attributes` `Regions` `Successors` `Properties` `Implements` `Assembly` `Custom` `Cases` `Groups` `Parameters` | 13 个 |
| Assembly 模式 | `attr-dict` `type` `if` | 3 个 |

三条从语料里读出来的规则：

1. **名字允许 `.`。** `reduce.return`（`corpus/scf.dialect:91`）、
   `forall.in_parallel`（`:51`）是真实助记符，不是嵌套语法。文法里
   `name : IDENT | name '.' IDENT`。
2. **`type` / `if` / `attr-dict` 只在 Assembly 模式里是关键字。** 因此
   `op if`（`scf.dialect:59`）、`type_def type`（`pdl.dialect:19`）在普通
   模式里完全合法。v1 把 `if` 做成全局关键字，`scf.if` 就写不出来。
3. **`unit` / `any` / `type` / `float` / `string` / `index` / `i1` 是原子，
   不是关键字。** 它们是类型世界里的标识符；做成关键字会让 `type_def type`
   失败，也会让 `Parameters (element: type)` 与声明名冲突。

## 3. 声明形态

| 实体 | 头部形态 | 计数 |
| --- | --- | --- |
| `dialect` | `dialect NAME { ... }` | 6 |
| `constraint` | `constraint NAME = TYPE ;` | 5 |
| `enum` | `enum NAME [ : bits ] { ... }` | 5（2 个 bits） |
| `schema` | `schema NAME [<params>] [extends REF] { clauses }` | 13 |
| `op` | `op NAME [from REF] { clauses }` | 77（45 个带 `from`） |
| `attr_def` / `type_def` | `attr_def NAME { ... }` | 1 / 4 |

三个值得注意的分布：

- **4 个 schema 有 extends 且空体**：`schema TotalIntBinary extends IntBinary { }`
  是合法定义——refinement 的语义就是"只加属性/接口，不加结构"。
- **2 个 schema 带类型参数**：`Compare<predicate_type: type>`、
  `Cast<source: type, target: type>`；参数有名字，实例化用位置实参
  （`Compare<CmpIPredicate>`）。
- **32 个 op 没有 `from`**：`constant`、`select`、`assert`、`br` 这类
  结构不共享的 op 直接定义，不需要为复用而硬造 schema。

## 4. 子句：哪类实体说哪类话

`tools/dialect-corpus.py` 的共现矩阵（数字是子句出现次数）：

| 实体 | Operands | Results | Attributes | Regions | Successors | Properties | Implements | Assembly | Doc | Cases | Groups | Parameters |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `op` | 28 | 21 | 17 | 10 | 3 | 41 | 11 | 32 | 77 | | | |
| `schema` | 5 | 5 | 6 | | | 5 | 6 | 8 | | | | |
| `enum` | | | | | | | | | 5 | 5 | 1 | |
| `attr_def` | | | | | | | | 1 | 1 | | | 1 |
| `type_def` | | | | | | | | 1 | 4 | | | 1 |

这张表直接决定了文法：`clause` 只允许这 9 种在 op/schema 里出现，
`enum` 只有 `Doc`/`Cases`/`Groups`，`attr_def`/`type_def` 只有
`Doc`/`Parameters`/`Assembly`。**没有一个实体用到了别的子句。**

绑定（binding）在语料里有五种形态：

| 形态 | 例子 | 计数 |
| --- | --- | --- |
| 裸类型 | `lhs: int_like` | 多数 |
| 依赖约束 | `result: type_of(lhs)` | 20+ |
| 带默认值 | `fastmath: FastMathFlags = none` | 12 |
| 可选 | `roundingmode: optional<RoundingMode>` | 11 |
| 可变 / 数组 / 嵌套 | `variadic<any>`、`array<integer>`、`variadic<variadic<any>>` | 34 / 6 / 1 |

结论：**元数是类型层的前缀构造子，不是后缀修饰符。** 语料里没有
`RoundingMode optional` 这种写法；v2 sketch 早期混用过，已统一为
`optional<T>`。

## 5. Assembly：语料只用了五个元素

| 元素 | 例子 | 计数 |
| --- | --- | --- |
| 字面量 | `","`、`":"`、`"fastmath"` | 多 |
| 部件引用 | `lhs`、`overflow_flags` | 多 |
| `attr-dict` | `attr-dict` | 24 |
| `type(x)` | `type(result)` | 35 |
| `if x { ... }` | `if overflow_flags { "overflow" overflow_flags }` | 16 |

语料**没有**用到 partial custom、`oilist`、`qualified`、`ref`、`enum`、
`functional-type`、整组指令（`operands`/`results`/`regions`/`successors`）。
所以 v2 文法里没有它们——这不是遗漏，是纪律：没有语料见证的构造不写。

`if` 是唯一带嵌套的文本构造，且**没有 `else`**。v1 凭直觉加了 `else`，
语料里 0 次使用；MLIR 的 optional group 也确实没有 else 分支。删掉。

## 6. 文法

47 条规则、98 条产生式（`bison --report=state`：`$accept` 编号 0，产生式编号 1–98）。按区域：

| 区域 | 行 | 产生式 |
| --- | --- | --- |
| 文件与方言 | `dialect-grammar.y:50` | `file`、`dialect_decl`、`dialect_item_list`、`dialect_item` |
| 名字与类型 | `:77` | `name`、`constraint_decl`、`type_expr`/`type_union`/`type_primary`、`type_ref` |
| schema / op | `:131` | `schema_decl`、`op_decl`、`type_params`、`param`、`extends`/`from` |
| 子句 | `:174` | `clause`、`binding`、`fact` |
| Assembly | `:226` | `assembly`、`asm_list`、`asm_element` |
| enum | `:248` | `enum_decl`、`enum_item`、`enum_case`、`enum_group` |
| attr / type | `:297` | `attr_type_decl`、`def_item` |
| 文档 | `:317` | `doc_clause` |

核心片段：

```yacc
name
  : IDENT
  | name DOT IDENT
  ;

schema_decl
  : KW_SCHEMA name type_params_opt extends_opt
    L_BRACE clause_list_opt R_BRACE
  ;

op_decl
  : KW_OP name from_opt L_BRACE clause_list_opt R_BRACE
  ;

clause
  : doc_clause
  | KW_OPERANDS   L_PAREN binding_list R_PAREN SEMI
  | KW_RESULTS    L_PAREN binding_list R_PAREN SEMI
  | KW_ATTRIBUTES L_PAREN binding_list R_PAREN SEMI
  | KW_REGIONS    L_PAREN binding_list R_PAREN SEMI
  | KW_SUCCESSORS L_PAREN binding_list R_PAREN SEMI
  | KW_PROPERTIES L_BRACKET fact_list R_BRACKET SEMI
  | KW_IMPLEMENTS L_BRACKET type_ref_list R_BRACKET SEMI
  | KW_ASSEMBLY assembly
  ;

asm_element
  : STRING
  | IDENT
  | A_ATTR_DICT
  | A_TYPE L_PAREN IDENT R_PAREN
  | A_IF IDENT L_BRACE asm_list R_BRACE
  ;
```

`constraint` 是唯一允许 `|` 的地方（类型集合的并集）；enum 组的成员也用
`|`，但那是另一条产生式（`name_group`），两条不共享。

## 7. 见证表

| 规则组 | 见证 |
| --- | --- |
| `file` / `dialect_decl` / `dialect_item_list` | 6 个方言 |
| `dialect_item` 的 7 个分支 | `Doc` 6、`constraint` 5、`enum` 5、`schema` 13、`op` 77、`attr_def` 1、`type_def` 4 |
| `name` 的两个分支 | `IDENT` 全部；`name '.' IDENT` 2（`scf:51,91`） |
| `constraint_decl` | 5 |
| `type_union` 的 `|` 分支 | `int_like`、`float_like`、`integer_or_index` |
| `type_primary` 的三分支 | 原子（多数）、`optional<T>` 11、`variadic<T>` 34 |
| `type_postfix_opt` 的三分支 | ε（多数）、`<...>`（`Cast<...>`、`array<...>`）、`(IDENT)`（`type_of`） |
| `type_arg_list` 的逗号分支 | `Cast<index, fixed_int_like>` |
| `type_ref` / `type_ref_args_opt` | `from IntBinaryWithOverflow`；`from Cast<index, ...>` |
| `type_ref_list` 的逗号分支 | `Implements [CallOpInterface, SymbolUserOpInterface]` |
| `schema_decl` | 13；其中空体 4、带参数 2、带 extends 8 |
| `param_list` 的逗号分支 | `schema Cast<source: type, target: type>` |
| `extends_opt` 的两分支 | 有 8、无 5 |
| `op_decl` / `from_opt` | 77；有 `from` 45、无 32 |
| `clause_list_opt` | 空 4（空体 schema）、非空 9 个 schema + 77 个 op |
| `clause` 的 9 个分支 | 见 §4 矩阵 |
| `binding` / `default_opt` | 裸类型/默认值/可选/可变均有；ε 见 `callee: symbol_ref` |
| `expr` | `none` |
| `fact` 的两分支 | `pure`；`has_parent(func)`、`parent_one_of(...)` |
| `assembly` 的两分支 | 块 24、`Custom` 18 |
| `asm_element` 的五分支 | 见 §5 |
| `enum_decl` | 5；`bits` 2、无 kind 3 |
| `enum_item` 的三分支 | `Doc` 5、`Cases` 5、`Groups` 1 |
| `enum_case` / `spelling_opt` | 有拼写（`0 "false"`）、无拼写（`0`） |
| `enum_group` / `name_group` | `fast = reassoc | ... | afn` |
| `attr_type_decl` | `attr_def` 1、`type_def` 4 |
| `def_item` 的三分支 | `Doc`、`Parameters` 2、`Assembly` 2 |
| `doc_clause` | 93 |

**所有 ε 分支也都有见证**：空 `type_postfix`（`lhs: int_like`）、空
`type_ref_args`（`from IntBinary`）、空 `type_params`（`schema FloatUnary`）、
空 `extends`/`from`、空 `clause_list`（`schema TotalIntBinary extends IntBinary { }`）、
空 `default`（`callee: symbol_ref`）、空 `enum_kind`、空拼写。

## 8. 语料逼出来的八个修正

| # | 修正 | 证据 |
| --- | --- | --- |
| 1 | op 名 = 助记符，允许 `.`；不再有 `op AddIOp "addi"` | `reduce.return`、`forall.in_parallel` 无法用单 IDENT 表达 |
| 2 | schema 没有 `mnemonic` 参数 | 77 个 op 的 `from` 里没有一个需要传助记符 |
| 3 | `optional`/`variadic` 一律前缀 | 语料里 11 个 `optional<T>`、34 个 `variadic<T>`，后缀写法 0 |
| 4 | 删除 Assembly 的 `else` | 0 次使用；MLIR optional group 无 else |
| 5 | `expr` 只保留 `IDENT` | 默认值只有 `none`；字符串/整数默认值 0 |
| 6 | `Groups` 一个子句一个组，成员用 `\|` | 单组；逗号成员与组列表在 LR(1) 下冲突（实测 1 个 S/R） |
| 7 | 空列表不写：`Operands ()` 非法，省略子句即可 | 空括号 0 次；省略子句是唯一写法 |
| 8 | 不写 `interface` 定义 | 四个方言只 `Implements`，没有定义；写了就是无见证产生式 |

第 6 条是唯一由**文法工具**（而不是语料语义）逼出来的：一开始写成
`Groups [ fast = a | b, other = c ]`，bison 报 `COMMA` 上的 S/R 冲突；
改成"一个子句一个组、组内只用 `|`"后 0 冲突，而且更符合领域事实——
组是命名并集，不是列表元素。

第 7 条是"一个概念一种写法"的直接结果：如果 `Operands ()` 和省略
`Operands` 都合法，就有两种方式说"没有 operand"。语料选了省略，文法也只
保留省略。

## 9. 机械核对

```bash
# 文法：0 冲突（-Werror 把冲突变成错误）
bison -Wall -Wcounterexamples -Werror -o /tmp/dg.c dialect-grammar.y

# 语料：6/6 解析通过
bison -d -o /tmp/dg.c dialect-grammar.y
cc -I/tmp -o /tmp/dg /tmp/dg.c tools/dialect-lex.c
/tmp/dg corpus/*.dialect

# 统计与审计
tools/dialect-corpus.py
```

结果：

| 检查 | 结果 |
| --- | --- |
| bison 冲突 | 0 |
| 语料解析 | 6/6 `parse OK` |
| `from`/`extends` 指向不存在的 schema | 0 |
| Assembly 引用了未声明的部件 | 0 |
| 声明后从未使用的 schema/constraint/enum | 0 |
| C++ 泄漏（`::`、`namespace`、`#include`、`$x`、`[{`） | 0 |

最后一项是本章的门禁：如果语料里出现 `::` 或 `$`，说明领域模型又被 codegen
污染了。

## 10. 与 v1 / v2 sketch 的差异

| 维度 | v1 `design-first-v1/` | v2 sketch | v2 文法（本章） |
| --- | --- | --- | --- |
| op 身份 | `op AddIOp "addi"` | `op addi` | `op addi`，允许 `.` |
| schema 参数 | 含 `mnemonic` | 无 | 无 |
| 可选写法 | `optional` 后缀 | 前缀 | 只允许前缀 |
| Assembly else | 有 | 有 | 无 |
| 空列表 | 允许 | 未定义 | 非法 |
| interface | 可定义 | 可定义 | 只可引用 |
| 关键字 | 全局 `if`（bug） | 未定义 | 仅 Assembly 模式 |
| 产生式来源 | 设计 | 单方言 | 6 方言语料 |

## 11. 局限与下一轮语料

1. **接口定义没有语法。** `Implements` 只引用外部接口；要定义接口，需要
   语料里有 method 列表（`LoopLikeOpInterface` 这类），这属于
   `mlir/Interfaces` 而不是方言。
2. **partial custom / `oilist` / 整组指令没有语法。** 语料里全部用
   `Assembly Custom;` 或逐部件写法；把 `linalg`/`gpu` 加进语料后才会出现。
3. **region 只有"有没有"和"可不可选"，没有块数。** `SizedRegion<1>` 的
   "恰好一块"目前只能写成 property `single_block`（op 级，不是 region 级）。
4. **没有 invariant 语言。** `has_parent` 用 property 表达；更复杂的
   父子/类型关系（`AllTypesMatch` 之外）还没有语料。
5. **没有模块、版本、import、条件编译。** 单文件一个方言，够用就好。

下一轮最值得加的语料是 `gpu`（interface 定义 + 多 region）与 `linalg`
（partial custom + `oilist` + YAML 生成的 named op）。每加一个，先写程序，
再让文法跟不上——跟不上才是新产生式的合法来源。

## 12. 结论

1. **文法可以归纳，但不能自动归纳。** 工具能做的是统计、冲突检测、见证
   对账；"哪些构造是同一件事"仍然是人判断的。
2. **语料优先修正了五个设计错误**：op 的双名字、schema 的 mnemonic 参数、
   后缀 optional、Assembly 的 else、空列表的两种写法。这些都是设计先行时
   "看起来合理"的决定。
3. **最硬的约束来自工具，不来自直觉。** `Groups` 的逗号写法在 LR(1) 下
   冲突，这是 bison 告诉我们的，不是语料告诉我们的；但它反过来让语言更
   贴近领域（组是命名并集）。
4. **0 冲突 + 6/6 解析 + 0 悬挂引用**，是"文法与语料一致"的可复现证据；
   任何一条不成立，说明归纳跑偏了。
5. **下一步不是扩文法，而是扩语料。** 文法已经覆盖 77 个 op、13 个 schema、
   5 个 enum、5 个 attr/type 定义；要继续长，就得先写新的方言程序。
