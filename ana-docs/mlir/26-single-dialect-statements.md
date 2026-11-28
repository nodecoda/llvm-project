# 26. 只描述一个方言：arith 的 8 类语句

> 配套：`arith-grammar.y`（186 行、22 个非终结符、8 类语句、bison 0 冲突）、
> `tools/arith-lex.c`（参考词法器 + 驱动）、`tools/arith-check.py`（语料 + 24 例自测）。
> 对照物：`dialect-grammar.y`（499 行、62 个非终结符），语料在 `corpus/`。
> 上游基线：`mlir/test/Dialect/Arith/` 的 32 个 `.mlir`、
> `mlir/include/mlir/Dialect/Arith/IR/ArithOps.td`（56 个助记符）、
> `mlir/lib/Dialect/Arith/IR/ArithOps.cpp`。
>
> 实测：**32 个文件、2930 个语句头、2922 条语句、8 条负语料被拒、
> 0 条意外失败、0 个文件丢语句**；自测 24 例全过。

## 0. 转向：从"定义方言的语言"到"一个方言自己的文本"

13–25 章一直在描述**定义语言**：ODS 语义文法（13 章）、方言领域模型（14 章）、
从 50 个方言的 `.dialect` 源程序反推的 `dialect-grammar.y`（15–25 章）。上一轮的
问题是"两阶段语法能不能拍平"，结论是：**能拍平表示，拍不平层次**——动作的嵌套
（`custom<...>`、`oilist`、块）和定义与文本的引用关系拍平后只能靠再准入约束去补，
语义反而更散。

这一轮把前提换掉：**不看 TableGen，不看别的方言，只描述 arith 的文本**。于是：

- 输入里只有 arith 的语句，其余（`func.func`、`scf.for`、`return`、
  `"test.foo"(...)`）一律当注释；
- `assemblyFormat` 的内容**就是语法的一部分**，而且是**语句**；
- 每个 op 对应**一类语句**，助记符就是这类语句的**引导词**——像 `if`/`while`/`print`。

这不是"把 `dialect-grammar.y` 简化"，而是把被宿主压扁的东西还原回来：在 ODS 里
`arith.addi` 的文本是字符串 `"$lhs `,` $rhs attr-dict `:` type($result)"`；在方言
自己的语言里，它就是一条语句的产生式。

## 1. 模型：助记符是关键字，形状是产生式

| 概念 | 方言语言里的角色 | 对应物 |
| --- | --- | --- |
| `arith.addi` | 引导词（关键字） | C 的 `if` / `while` |
| 8 类语句 | 产生式 | C 的 `if_stmt` / `while_stmt` |
| `arith.addi` 与 `arith.subi` | 两个关键字、同一条产生式 | `if` 与 `while` 共用 `"(" e ")" s` 的形状 |

"每个 op 对应一类语句"要读准：**每个 op 是一条语句（有自己的引导词），但不是
每个 op 独占一条产生式**。形状相同的 op 共享产生式，这和 C 里 `break;` 与
`continue;` 共用一条语句产生式、`if` 与 `while` 共用条件加体的形状是同一件事。
56 个引导词、8 条产生式，不矛盾。

规模：`ArithOps.td` 里 56 个助记符字符串，与参考词表的 56 条 `MNEMONICS[]`
一一对应（缺一条就会有语句被静默丢掉，§9 的检查正是为此）。

arith 的另一个事实让这个模型**恰好精确**：**arith 没有任何 region**
（`ArithOps.td` 里 `hasRegion` 出现 0 次），所以 arith 语句永不嵌套，一个程序
就是一个**平的语句序列**。这也是 8 类语句就够的原因；`scf` 那种带 region 的
方言必须回到嵌套模型。

## 2. 8 类语句与 56 个助记符

| 语句类 | 助记符数 | 成员 | 语料条数 | ODS 依据 |
| --- | --- | --- | --- | --- |
| `OP_BINARY` | 31 | `addf addi andi ceildivsi ceildivui divf divsi divui floordivsi maximumf maximumnumf maxnumf maxsi maxui minimumf minimumnumf minnumf minsi minui mulf muli ori remf remsi remui shli shrsi shrui subf subi xori` | 959 | `Arith_BinaryOp`（`ArithOps.td:52`）、整数/浮点/overflow/exact/rounding 变体（`:58`、`:193`、`:207`、`:96`） |
| `OP_CAST` | 13 | `bitcast convertf extf extsi extui fptosi fptoui index_cast index_castui sitofp truncf trunci uitofp` | 605 | `Arith_CastOp`（`ArithOps.td:146`，格式 `:152`）；`trunci` 另有 overflow 槽位（`:1571`） |
| `OP_CONST` | 1 | `constant` | 883 | `Arith_ConstantOp`（`ArithOps.td:223`，格式 `:263`） |
| `OP_CMP` | 2 | `cmpi cmpf` | 264 | `Arith_CompareOp`（`ArithOps.td:184`，格式 `:190`）；`cmpf` 的 fastmath 在 `:2055` |
| `OP_SELECT` | 1 | `select` | 78 | `SelectOp`（`ArithOps.td:2071`）——**没有 `assemblyFormat`**，见 §7 |
| `OP_MULTI` | 4 | `addui_extended mulsi_extended mului_extended subui_extended` | 70 | `ArithOps.td:314`、`:365`、`:511`、`:554` |
| `OP_UNARY` | 2 | `flush_denormals negf` | 35 | `Arith_UnaryOp`（`ArithOps.td:46`）、`Arith_FloatUnaryOp`（`:69`） |
| `OP_SCALING` | 2 | `scaling_extf scaling_truncf` | 28 | `ArithOps.td:1504`、`:1690` |

合计 56 个助记符、2922 条语句。分布（`--trace` 实测）：

```text
binary 959 / const 883 / cast 605 / cmp 264 / select 78 / multi 70 / unary 35 / scaling 28
```

两个注意点：

1. **语句类不等于基类**。`arith.divsi` 继承 `Arith_IntBinaryOpWithExactFlag`
   （`ArithOps.td:207`，比 `Arith_BinaryOp` 多一个 `exact` 槽位），但它是二元语句的
   一个**变体**，不是第九类。类由形状里的**必选部分**定，可选槽位不进分类。
2. **`select` 证明分类是从文本反推的**。它没有 `assemblyFormat`，文本由
   `SelectOp::parse` 手写（`ArithOps.cpp:3368`），但它的文本仍然是
   `%cond, %t, %f (attr-dict)? ":" type ("," type)?` 的同一种形状。分类若是照抄
   ODS，`select` 只能进"无格式"桶。

## 3. 文法

完整产生式见 `arith-grammar.y`；这里只列骨架。序言一段注释交代模型与五个
外来槽位（§5），文法本体 22 个非终结符：

```yacc
unit      : statement loc_opt ;              /* 一条语句；序列在驱动里（§9） */
statement : opt_results OP_CONST   const_tail
          | opt_results OP_UNARY   unary_tail
          | opt_results OP_BINARY  binary_tail
          | opt_results OP_CMP     cmp_tail
          | opt_results OP_SELECT  select_tail
          | opt_results OP_CAST    cast_tail
          | opt_results OP_MULTI   multi_tail
          | opt_results OP_SCALING scaling_tail ;

const_tail   : attr_dict_opt cvalue opt_type ;                       /* 值在类型前 */
unary_tail   : SSA opt_fastmath attr_dict_opt COLON TYPE ;
binary_tail  : SSA COMMA SSA opt_overflow opt_exact opt_round opt_fastmath
               attr_dict_opt COLON TYPE ;
cmp_tail     : predicate COMMA SSA COMMA SSA opt_fastmath
               attr_dict_opt COLON TYPE ;
select_tail  : SSA COMMA SSA COMMA SSA attr_dict_opt COLON TYPE opt_type_tail ;
cast_tail    : SSA opt_nneg opt_overflow opt_round opt_fastmath
               attr_dict_opt COLON TYPE KW_TO TYPE ;
multi_tail   : SSA COMMA SSA attr_dict_opt COLON TYPE opt_type_tail ;
scaling_tail : SSA COMMA SSA opt_round opt_fastmath
               attr_dict_opt COLON TYPE COMMA TYPE KW_TO TYPE ;

predicate    : PRED | STRING | BOOL ;        /* sle / "eq" / true */
cvalue       : VALUE | BOOL ;                /* 42 / dense<...> / true */
opt_type     : %empty | COLON TYPE ;
opt_type_tail: %empty | COMMA TYPE ;
opt_results  : %empty | RESULTS ;            /* `%a =` / `%a, %b =` */
opt_fastmath : %empty | KW_FASTMATH FLAGS ;
opt_overflow : %empty | KW_OVERFLOW FLAGS ;
opt_exact    : %empty | KW_EXACT ;
opt_nneg     : %empty | KW_NNEG ;
opt_round    : %empty | ROUND ;
attr_dict_opt: %empty | ATTRDICT ;
```

`bison -Wall -Wcounterexamples -Werror` 对这份文法报 **0 冲突**。几个设计点：

- **旗标槽位顺序固定**（overflow → exact → roundingmode → fastmath），因为上游
  格式串就是这个顺序（`ArithOps.td:203`、`:215`、`:126`、`:90`）。写成集合会接受
  `exact overflow<nsw>` 这种上游打印不出来的文本。
- **`opt_type` 只在常量上与多结果上可选**，其余语句类型必填：每一条非常量
  语句的格式串都以 `` `:` type(...) `` 结尾（`ArithOps.td:54`、`:152`、`:190`），
  没有"省略类型"的写法；语料里也没有反例。
- **`cvalue` 不是 `TYPE`**。常量值可以是数、`dense<...>`、`dense_resource<...>`、
  字符串、布尔，但不能是 SSA 名——`arith.constant %x` 是负例
  （`invalid.mlir:923`，报文落在 `:925` 的 `%x` 上）。

## 4. 语料逼出的事实

每一条都至少有一个正例或负例；除特别说明外，路径相对 `mlir/test/Dialect/Arith/`。

| # | 事实 | 见证 |
| --- | --- | --- |
| 1 | 语句边界由文法决定，不由换行或括号平衡 | `canonicalize.mlir:5607-5608`：`%low, %bo = arith.subui_extended ...` 的类型列表在下一行 |
| 2 | 操作数列表不能加括号 | `invalid.mlir:135` → `expected SSA operand` |
| 3 | 操作数列表不能加花括号 | `invalid.mlir:142` |
| 4 | 操作数个数是精确的 | `invalid.mlir:128` → `expected ':'` |
| 5 | 比较谓词不可省 | `invalid.mlir:245` → `expected string or keyword containing one of the following enum values` |
| 6 | 谓词可以加引号 | `canonicalize.mlir:3120`：`arith.cmpi "eq", %arg0, %arg1 : i8` |
| 7 | `true`/`false` 也是 CmpF 谓词拼写 | `canonicalize.mlir:4572`：`arith.cmpf true, %fp, %zero : f32` |
| 8 | 常量可以没有 `: type` | `canonicalize.mlir:118-119`：`arith.constant false` / `true`（i1 在值里） |
| 9 | 常量值不能是 SSA 名 | `invalid.mlir:923` → `expected attribute value` |
| 10 | 结果个数不由类型列表推出 | `canonicalize.mlir:2918`（`mulsi_extended`：2 结果、1 类型）对 `:2688`（`addui_extended`：2 结果、2 类型） |
| 11 | 结果前缀有第三种拼写 `%x:2 =` | `invalid.mlir:157`：`%r:2 = arith.addui_extended %a, %a : f32, i32` |
| 12 | 舍入模式是 5 个裸词，含 `to_nearest_away` | `ops.mlir:804`：`arith.truncf %arg0 to_nearest_away : f64 to f32` |
| 13 | attr-dict 可以跟在旗标之后 | `unsigned-when-equivalent.mlir:53`：`arith.divsi %1, %c4 exact {test.marker} : i32` |
| 14 | 类型可以是函数类型 | `invalid.mlir:294`：`arith.cmpi eq, %idx, %idx : () -> ()` |
| 15 | `dense_resource<...>` 是一种值 | `canonicalize.mlir:5376`：`arith.constant dense_resource<blob1> : tensor<i8>` |
| 16 | SSA 名可以以 `-` 开头 | `canonicalize.mlir:3650`：`%-inf = arith.constant 0xFF800000 : f32` |
| 17 | `scaling_*` 有**两个**输入类型 | `canonicalize.mlir:1176`：`arith.scaling_extf %in, %scale : f4E2M1FN, f8E8M0FNU to f32` |
| 18 | generic 形式不是方言语句 | `ops.mlir:1198`：`%0 = "arith.constant"(){value = 42 : i32} : () -> i32`（§6） |

事实 10 值得单独说：`multi_tail` 的第二个类型是**可选的**，这不是宽松，而是两种
op 的真实现状。`mulsi_extended` 的第二个结果类型由第一个决定（同宽），所以文本
只写一个类型；`addui_extended` 的第二个结果是 i1 形状，独立打印。若强行统一，
要么丢信息，要么让 `mulsi_extended` 打印冗余类型。

## 5. 五个外来槽位

这五个东西**不属本方言的语法**，但它们的**形状**必须被约束，否则文法会被文本
里的标点骗走：

| 槽位 | 形状 | 谁定义 | 约束方式 |
| --- | --- | --- | --- |
| `TYPE` | `i32`、`vector<8xi64>`、`tensor<?xi32>`、`() -> ()` | builtin | 括号配平（`<`/`(`）；`->` 后可再接函数类型 |
| `VALUE` | 数（含 `0x`）、`dense<...>`、`dense_resource<...>`、字符串、`true`/`false` | builtin 属性 | `<>` 配平；不能是 `%name` |
| `ATTRDICT` | `{ ... }` | builtin 字典属性 | 花括号配平 + **内容里不能有 `%` 或 `^`**（§9） |
| `FLAGS` | `fastmath<...>`、`overflow<...>` | arith 枚举 | 词法器逐个校验旗标名（`nsw`/`nuw`/`fast`/...） |
| `LOC` | `loc(...)` | builtin | 圆括号配平；只允许出现在语句末尾 |

"外来"不等于"不管"。旗标名写错（`fastmath<bogus>`）必须报错，否则 `FLAGS` 就是
黑洞；同理 `ATTRDICT` 必须能区分"属性字典"和"别的方言的 region 体"，否则
语句会被整段吞掉（§9 的两个 bug 都是这条）。

## 6. 两条通道：custom 与 generic

MLIR 的 IR 语言给所有 op 一条通用语句：

```mlir
%0 = "arith.constant"(){value = 42 : i32} : () -> i32     // ops.mlir:1198
```

引号里的名字 + 括号操作数 + 字典属性 + 函数类型。它**不是 arith 的语句**，
而是 IR 语言对所有 op 的同一条语句。本方言的语言把它当注释，理由和
`func.func` 一样：它属于外层语言。

语料里 18 处 generic 形式（`grep -c '"arith\.' mlir/test/Dialect/Arith/*.mlir`
合计 18），全部当注释跳过；custom 形式 2922 条，全部被文法接住。

这条边界同时解释了 `invalid.mlir` 的结构：145 条 `expected-error` 里，只有 8 条
是**语法层**（被参考解析器拒），其余挂在 124 条能解析的语句上——那些错在
**verifier 层**（位宽、形状、谓词与操作数类型不匹配）。语法正确、语义错误，
这正是两层该有的分工。

## 7. "每个 op 一类语句"在哪里成立

成立的部分（§1）：每个 op 有独立引导词；语句类由形状决定。

不成立的部分，三个反例：

1. **家族压缩**：31 个二元 op 共享一条产生式。它们的差别在词表（助记符）和
   可选槽位，不在形状。若强行"一 op 一产生式"，会得到 31 条只差一个终结符的
   产生式，LR 表翻倍而语言不变。
2. **`select` 没有 `assemblyFormat`**。文本在 `SelectOp::parse`
   （`ArithOps.cpp:3368`）：三个操作数、可选属性字典、`:`、结果类型，**若条件
   是 shaped 才**再跟一个条件类型（`:3377` 用 `parseOptionalComma` 实现）。
   这段 C++ 手写的文本仍然落在 `OP_SELECT` 一类里——分类来自文本，不来自 ODS。
3. **可选槽位不是语句类**：`overflow`、`exact`、`roundingmode`、`fastmath`
   是槽位；`divsi exact` 与 `divsi` 是同一条语句的两种拼写，不是两类。

所以准确的说法是：**op 是语句，形状是类**。56 个语句、8 个类，和 C 的
"关键字多、语句产生式少"同构。

## 8. 与 `dialect-grammar.y` 的对比

| 维度 | `dialect-grammar.y`（15–25 章） | `arith-grammar.y`（本章） |
| --- | --- | --- |
| 描述对象 | 方言的**定义**（op/type/attr/enum/interface） | 一个方言的**文本语句** |
| 规模 | 499 行、62 个非终结符 | 186 行、22 个非终结符 |
| 输入 | 50 个方言、2777 个 op 的 `.dialect` 源程序 | 32 个 `.mlir`、56 个助记符、2922 条语句 |
| 复杂度来源 | **跨方言**形状差异（`custom`/`oilist`/`ref`/块/逃逸点） | **单方言**内部的可选槽位 |
| 序列 | 由文件结构给出（实体序列） | 由驱动给出（语句序列，§9） |
| 没覆盖的 | 文本（交给 Assembly 三通道） | 定义（不关心）、别的方言（当注释） |

不是为了比大小。要点是：**复杂度的来源换了一层**。`dialect-grammar.y` 之所以
需要 62 个非终结符，是因为它要同时容纳 50 个方言的**形状分歧**；arith 一个方言
内部没有形状分歧，只有槽位分歧，于是 8 条语句就够。换成 `scf`（带 region）或
`linalg`（`oilist`、YAML named op），8 条会不够——不够的部分才是新产生式的
合法来源（与 21–22 章的判据一致：语料逼出来的才算）。

## 9. 验证：三个检查，一条可证伪的不变量

```bash
cd ana-docs/mlir
bison -Wall -Wcounterexamples -Werror -o /tmp/ag.c arith-grammar.y   # 0 冲突
python3 tools/arith-check.py                                        # 语料 + 自测
```

`arith-check.py` 做三件事：

1. **语料不误伤**：每条被拒的语句，上游必须在 4 行内留 `expected-error`；
   否则算"意外失败"。
2. **不静默丢语句**（本轮新增，也是最有价值的一条）：逐文件比较
   **源文本里的语句头数**（正则数，与解析器无关）与**解析出的语句数 + 诊断数**，
   必须相等。`2930 = 2922 + 8`，32 个文件逐个成立。
3. **24 例自测**：§4 的正负例，加 attr-dict、loc、`dense_resource`、函数类型、
   `({ ... })` region、`{-# ... #-}` 资源块、generic 形式、带 `%` 的字符串属性。

第 2 条不是装饰。它上线第一次跑就抓出**两个参考词法器的 bug**，共 12 条语句
被静默吞掉（2910 → 2922，正好是下面的 7 + 5）：

| bug | 症状 | 根因 | 修法 |
| --- | --- | --- | --- |
| `{` 紧跟 SSA | `scf.for ... %c64 {` 的循环体整段消失（`int-range-narrowing.mlir:728` 起 4 条、`unsigned-when-equivalent.mlir:80-82` 3 条，共 7 条） | `{` 一律当 attr-dict，`balanced('{','}')` 把 region 体跳完 | `brace_is_dict()`：内容里出现 `%`/`^` 就不是字典 |
| `({ ... })` | `"test.region"(%a) ({ ^bb0: ... })` 的体内语句消失（`select-common-elementwise.mlir:62` 起 4 条、`int-range-opts-crash.mlir:9`，共 5 条） | 任何 `(` 都当函数类型，整段配平 | `(` 后紧跟 `{` 时只消耗 `(`，让体内语句照常切分 |

两个 bug 的形状相同：**"外来槽位"的配平规则把方言语句吞了**。这正是 §5 那条
纪律的由来——外来槽位可以不解内部，但必须能判"什么时候结束"，否则它就是黑洞。

## 10. 诚实的边界

1. **只宣称 `C ⊆ L(G)`，不宣称相等**。2922 条语句全部通过、8 条负例全部被拒；
   这证明"语料在内、已知的错误在外"，不证明"文法就是方言的真语法"。要更强，
   得先立规则再证伪（上一轮讨论的路线），而不是继续加语料。
2. **精确与紧凑二选一**。当前写法接受 `%r:2 =`（事实 11）却不把它算进
   `opt_results`；接受 `%-inf` 这类 SSA 名却不校验它是否已定义。SSA 的绑定
   语义不在本层，`%x = arith.addi %x, %x` 在语法上合法。
3. **generic 形式不覆盖**（§6，18 处）。它们是 IR 语言的语句，不是方言的。
4. **别的方言一律当注释**是设计代价：`func.func`、`scf.for` 的括号错配不会被
   本文法发现；本层只保证"被识别为 arith 语句的那些片段"自洽。
5. **嵌套不支持**：arith 无 region（§1），所以平序列模型精确；换成 `scf`
   必须把 `region` 加回文法，8 类语句会变成"语句 + 语句块"。
6. **词法层的宽容**：`0x` 十六进制、指数浮点、下划线数字都按"一个值"扫过，
   不校验数值范围；`i16777216` 这种位宽上限错误留给 verifier
   （`invalid.mlir:120` 正是这一层）。

一句话：**这份文法能用的范围是"arith 的 custom 文本"，它能证明的是"包含"，
不能证明的是"正确"。**

## 11. 复现

```bash
cd ana-docs/mlir
python3 tools/arith-check.py            # 期望最后两行：
                                        # 语料: 32 文件, 2930 个语句头, 2922 条语句,
                                        #       8 条负语料被拒, 0 条意外失败, 0 个文件丢语句
                                        # 自测: 24 例, 0 例失败

# 看每条语句被判成哪一类（行号 + 类 + 助记符）
D=/tmp/arith-build; mkdir -p $D
bison -d -o $D/ag.c arith-grammar.y
cc -I$D -O1 -Wall -Wextra -Werror -o $D/arith $D/ag.c tools/arith-lex.c
$D/arith --trace ../../mlir/test/Dialect/Arith/ops.mlir | head

# 看 token 流（ARITH_TOK=1），调试"为什么这条没被认成语句"
ARITH_TOK=1 $D/arith /tmp/one.mlir 2>&1 | head -40
```
