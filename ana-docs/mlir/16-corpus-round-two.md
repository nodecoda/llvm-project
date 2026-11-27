# 16. 第二轮语料：gpu / linalg / tensor，以及谓词的组合

> 配套文件：
> [`corpus/gpu.dialect`](corpus/gpu.dialect)、[`corpus/linalg.dialect`](corpus/linalg.dialect)、
> [`corpus/tensor.dialect`](corpus/tensor.dialect)（合计 499 行、42 个 op）；
> [`dialect-grammar.y`](dialect-grammar.y)（**55 条规则 / 128 条产生式**，
> bison `-Wall -Wcounterexamples -Werror` **0 冲突**）；
> [`tools/dialect-corpus.py`](tools/dialect-corpus.py)、
> [`tools/dialect-lex.c`](tools/dialect-lex.c)（9/9 语料 `parse OK`）。
>
> 与 15 章的关系：15 章用 6 个方言归纳出第一版文法；本章用 3 个新方言
> 把它推过边界，并顺带回答"谓词能不能组合"。

## 0. 方法：让方言逼文法

15 章的纪律不变：**先写方言源程序，再让文法跟不上**。本轮新增的三个方言
各自带来一类此前没有的构造：

| 方言 | 它能逼出什么 | 为什么不是 arith/scf/func/cf |
| --- | --- | --- |
| `gpu` | `oilist`、`attr-dict-with-keyword`、`functional-type`、整数默认值 | kernel 内建与同步原语有互斥可选组 |
| `linalg` | 命名算符家族、整数谓词实参 `inputs(2)`、region 单块约束 | 60+ 个 op 由同一份结构化描述生成 |
| `tensor` | partial custom `custom<Hook>(...)`、谓词析取 | 切片类 op 的混合静态/动态列表是局部子格式 |

一次只加一类新构造，加完立刻跑文法与工具；冲突或悬挂引用出现，说明
归纳方向错了。

## 1. 语料

| 文件 | 行数 | dialect | constraint | enum | schema | op | attr_def | type_def |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `arith.dialect` | 401 | 1 | 4 | 5 | 13 | 56 | | |
| `scf.dialect` | 128 | 1 | 1 | | | 12 | | |
| `gpu.dialect` | 246 | 1 | 2 | 4 | | 18 | | |
| `linalg.dialect` | 127 | 1 | | 1 | 1 | 12 | | |
| `tensor.dialect` | 126 | 1 | | | | 12 | | |
| `func.dialect` | 48 | 1 | | | | 5 | | |
| `cf.dialect` | 44 | 1 | | | | 4 | | |
| `pdl.dialect` | 22 | 1 | | | | | | 4 |
| `complex.dialect` | 10 | 1 | | | | | 1 | |
| **合计** | **1152** | **9** | **7** | **10** | **14** | **119** | **1** | **4** |

三个方言的领域贡献：

- **gpu**：18 个 op，覆盖 launch/launch_func 的分裂、kernel 内建
  （`block_id`/`thread_id`/`block_dim`/`grid_dim`）、同步（`barrier`）、
  集团操作（`shuffle`/`all_reduce`/`subgroup_reduce`/`warp_execute_on_lane_0`）、
  `module`/`func` 的符号与区域。
- **linalg**：1 个 schema `Structured` + 5 个命名算符（`copy`/`fill`/`add`/
  `matmul`/`batch_matmul`），再加 generic 形态（`generic`/`map`/`reduce`/
  `transpose`/`broadcast`）与 `index`/`yield`。
- **tensor**：12 个 op，覆盖元素访问、切片、填充、生成；切片类 op 的
  偏移/尺寸/步长是"静态数组 + 动态 operand"两份数据。

## 2. 十个由语料逼出的扩展

| # | 构造 | 语料见证 | 真实出处 | 文法动作 |
| --- | --- | --- | --- | --- |
| 1 | `oilist(a \| b \| c)` | `gpu.barrier` 的 `named`/`memfence`/`scope` | `GPUOps.td:1524` | 新增 `oilist_alt_list`，分支用 `\|` |
| 2 | `attr-dict-with-keyword` | `gpu.module` | `GPUOps.td:1626` | 新增 `A_ATTR_DICT_WITH_KEYWORD` |
| 3 | `functional-type(operands, results)` | `gpu.all_reduce`、`gpu.subgroup_reduce`、`tensor.reshape` | `GPUOps.td:1231`、`:1311`、`TensorOps.td:1049` | 新增 `A_FUNCTIONAL_TYPE`，实参是部件组 |
| 4 | `custom<Hook>(a, b)` | `tensor.extract_slice`/`insert_slice`/`pad` | `TensorOps.td:443-445`、`:900-902`、`:1365-1366` | 新增 `A_CUSTOM` + `custom_arg_list` |
| 5 | region 注解 `[single_block]` | 19 处（见 §4） | `SCFOps.td:285` 等 | 新增 `region_binding` / `region_annots_opt` |
| 6 | 整数默认值 `= 1` | `gpu.subgroup_reduce.cluster_stride` | `GPUOps.td:1280` | `expr : IDENT \| INT` |
| 7 | 谓词析取 `\|` | `tensor.yield` 的两个允许父 op | `TensorOps.td:1832` | `fact : fact_atom \| fact PIPE fact_atom` |
| 8 | 谓词整数实参 | `linalg` 命名算符的 `inputs(2)` | `core_named_ops.py` 的固定 arity | `fact_arg : name \| INT` |
| 9 | 软关键字 | `gpu.all_reduce` 的属性名 `op` | `GPUOps.td:1223` | `name : IDENT \| decl_keyword` |
| 10 | 名字里的 `.` 只在词法 | `reduce.return`、`forall.in_parallel` | `SCFOps.td` 助记符 | 删除 `name '.' IDENT` 死产生式 |

第 10 条不是新构造，是**修正**：15 章声称 `name : IDENT | name '.' IDENT`，
但词法器把点号折进同一个 `IDENT` token，DOT 分支从未被触发。语法应当
描述词法器真正产生的 token 序列，所以删掉它，把"."属于名字"写在词法层。

## 3. 谓词的组合

问题：`Properties [ ... ]` 里的谓词能不能用 `and`/`or` 组合？

答案：**能，但只有一个组合符号 `|`；合取是列表本身。**

```yacc
fact_list
  : fact
  | fact_list COMMA fact          /* 列表 = 合取 */
  ;

fact
  : fact_atom
  | fact PIPE fact_atom           /* `|` = 析取 */
  ;

fact_atom
  : IDENT
  | IDENT L_PAREN fact_arg_list R_PAREN
  ;
```

三个决定，各有语料或反例支撑：

1. **不用 `and`。** 列表已经是合取。`(a ∨ b) ∧ c` 写成 `[a | b, c]`，
   不需要第二个合取符号——"一个概念一种写法"。
2. **不用 `or` 关键字。** `gpu.all_reduce` 的枚举 case 就叫 `and`/`or`/`xor`
   （`GPUOps.td:1151-1153`）。把它们变成保留字，`Cases [ ... or = 9 "or" ... ]`
   立刻写不出来；15 章已经为 `type`/`if` 立过同样的规矩：关键字只在必要的
   上下文里是关键字。`|` 在类型并集（`type_union`）与 enum 组合
   （`name_group`）里已经承担"并/或"，谓词沿用它，语义一致。
3. **没有 `not`。** 没有任何语料需要否定一个结构事实；而且"不是某形状"
   通常应该换一个正向谓词，而不是给谓词加否定。

代价是 `a ∨ (b ∧ c)` 目前写不出来（列表只能表达外层合取）。没有语料见证
之前不加括号与 `and`；等真出现这种约束，再按同一纪律扩展。

语料里的 `parent_one_of(a, b, ...)` 因此被删除，替换成可组合的析取：

```text
scf.yield:  has_parent(execute_region) | has_parent(for) | has_parent(if)
          | has_parent(index_switch) | has_parent(while)
tensor.yield: has_parent(generate) | has_parent(pad)
```

`parent_one_of` 是 `HasParent` 的"多个实参"特例（`SCFOps.td:1244`），
它本来就是一个析取；换成 `|` 之后，谓词集合少了一个特殊形式，
工具里的 `facts` 计数也从 `parent_one_of=1` 变成 `has_parent=14`。

## 4. region 约束搬家：从 op 到 region

15 章把"恰好一块"写成 op 属性 `single_block`。本轮把它移到 region 绑定上：

| op | 15 章写法 | 16 章写法 | ODS 事实 |
| --- | --- | --- | --- |
| `scf.for` | `Properties [single_block, ...]` | `Regions (body: region [single_block])` | `SCFOps.td:285` |
| `scf.if` | 同上 | 两个 region 各带 `[single_block]` | `:777` |
| `scf.reduce` | 未标 | `reductions: variadic<region> [single_block]` | `:990` |
| `gpu.warp_execute_on_lane_0` | — | `warp_region: region [single_block]` | `GPUOps.td:3343` |
| `gpu.module` | — | `body: region [single_block]` | `:1622` |
| `linalg.map`/`reduce`/`transpose`/`broadcast` | — | 各自的 region `[single_block]` | `LinalgStructuredOps.td:274,362,427,504` |
| `tensor.generate`/`pad` | — | `body`/`region` `[single_block]` | `TensorOps.td:754,1357` |

两个反例同样重要——**它们是 AnyRegion，不能标 `single_block`**：

- `gpu.launch` 的 body 是 `AnyRegion`（`GPUOps.td:947`）；
- `gpu.func` 的 body 是 `AnyRegion`（`:438`）；
- `func.func` 的 body 也是 `AnyRegion`（`FuncOps.td:313`）。

我最初按直觉给 `gpu.launch` 和 `func.func` 标了 `single_block`，核对 ODS
后撤回。这条比任何"看起来合理"的推断都硬：**约束必须来自领域事实，
不是来自对 IR 的想象。**

## 5. 软关键字：`op` 为什么能当属性名

`gpu.all_reduce` 的属性在 ODS 里叫 `$op`（`GPUOps.td:1223`）：

```td
OptionalAttr<GPU_AllReduceOperationAttr>:$op
```

如果 `op` 是硬关键字，语料里就写不出 `Attributes (op: optional<...>)`。
解决方式不是改属性名，而是把声明关键字降级为**软关键字**：

```yacc
name
  : IDENT
  | decl_keyword
  ;

decl_keyword
  : KW_DIALECT | KW_OP | KW_SCHEMA | KW_CONSTRAINT | KW_ENUM
  | KW_ATTR_DEF | KW_TYPE_DEF | KW_FROM | KW_EXTENDS | KW_BITS
  | KW_OPTIONAL | KW_VARIADIC
  ;
```

这些词只在声明/子句的起点有意义；到了名字位置，它们仍是名字。文法仍然
0 冲突，因为每个声明分支的第一个 token 已经决定了走哪条产生式。

## 6. 文法增量

| 指标 | 15 章 | 16 章 |
| --- | --- | --- |
| 规则（左部非终结符） | 47 | **55** |
| 产生式（bison 编号） | 98 | **128** |
| bison 冲突 | 0 | **0** |
| 语料文件 | 6 | **9** |
| 解析通过 | 6/6 | **9/9** |

新增规则 9 个：`decl_keyword`、`region_binding_list`、`region_binding`、
`region_annots_opt`、`custom_arg_list`、`oilist_alt_list`、`fact_atom`、
`fact_arg_list`、`fact_arg`；删除 1 个：`name_list`。

## 7. 见证表

| 新产生式 | 见证 | 计数 |
| --- | --- | --- |
| `A_OILIST` | `gpu.barrier` | 1 |
| `A_ATTR_DICT_WITH_KEYWORD` | `gpu.module` | 1 |
| `A_FUNCTIONAL_TYPE` | `gpu.all_reduce`、`gpu.subgroup_reduce`、`tensor.reshape` | 3 |
| `A_CUSTOM` | `tensor.{extract_slice,insert_slice,pad}` | 8 个钩子 |
| `region_annots_opt` | scf/gpu/linalg/tensor 的 19 个 region | 19 |
| `fact PIPE fact_atom` | `tensor.yield`、迁移后的 `scf.yield` | 2 个 op |
| `fact_arg INT` | `linalg` 的 `inputs(2)`/`outputs(1)` | 5 个 op |
| `expr INT` | `gpu.subgroup_reduce.cluster_stride = 1` | 1 |
| `decl_keyword`（作为名字） | `gpu.all_reduce` 的 `op` | 1 |
| `custom_arg_list` | 三个 tensor 切片 op | 8 |

工具统计（`tools/dialect-corpus.py`）：

```text
assembly: type=61, attr-dict=52, if=34, custom=8, operands=5,
          functional-type=3, results=3, attr-dict-with-keyword=1, oilist=1
region facts: single_block=19
type ctors: variadic=68, optional=55, array=28
facts: has_parent=14            (parent_one_of 已删除)
```

## 8. 机械核对

```bash
bison -Wall -Wcounterexamples -Werror -o /tmp/dg.c dialect-grammar.y
bison -d -o /tmp/dg.c dialect-grammar.y
cc -I/tmp -o /tmp/dg /tmp/dg.c tools/dialect-lex.c
/tmp/dg corpus/*.dialect
python3 tools/dialect-corpus.py
```

| 检查 | 结果 |
| --- | --- |
| bison 冲突 | 0 |
| 语料解析 | 9/9 `parse OK` |
| `from`/`extends` 指向不存在的 schema | 0 |
| Assembly 引用未声明部件 | 0 |
| 声明后未使用的 schema/constraint/enum | 0 |
| C++ 泄漏（`::`、`namespace`、`#include`、`$x`、`[{`） | 0 |

工具本轮增加了两项：`custom<Hook>` 的钩子名不算部件引用（否则
`DynamicIndexList` 会被报成悬挂引用）；region 注解单独计数。

## 9. 局限与下一轮

1. **`a ∨ (b ∧ c)` 写不出来。** 需要括号 + `and`；等语料见证。
2. **零实参 `custom<Hook>` 没有语法。** 本轮所有钩子都有实参；tensor 之外
   （memref/vector）可能出现零实参形式。
3. **`functional-type` 只接受两个组名。** 真实格式里还可以是
   `functional-type(operands, results)` 之外的组合；等见证再加。
4. **interface 定义仍不在方言层。** gpu/linalg 只 `Implements`；接口方法表
   属于 `mlir/Interfaces`。
5. **命名算符的 provenance 不进语料。** linalg 的 60+ 个命名算符由
   Python→YAML→TD 生成；语料只记录"同一 schema + 定元事实"这个领域事实。
6. **region 的块数只有"恰好一块"。** `MinSizedRegion`/`MaxSizedRegion`
   还没有语料；pdl_interp 的 `MinSizedRegion<1>` 是下一个候选。

下一轮语料：`memref`（`custom<GlobalMemrefOpTypeAndInitialValue>`、
零实参钩子）、`vector`（更多 `oilist` 变体）、`affine`（映射/整数集属性）。

## 10. 结论

1. **新构造只能由新方言逼出来。** 3 个方言换来 10 项扩展；其中 6 项
   （oilist、attr-dict-with-keyword、functional-type、custom、整数默认值、
   整数谓词实参）在此前的 6 个方言里一次都没出现。
2. **谓词的组合应当是逻辑本身，不是特殊谓词。** 删除 `parent_one_of`、
   引入 `|` 之后，谓词集合更小，表达力更大；拼写选 `|` 而不是 `or`，
   是因为 `or` 是 `gpu.all_reduce` 的合法 case 名。
3. **约束要挂在它约束的实体上。** `single_block` 从 op 搬到 region，
   才能表达"变长 region 组里每个 region 都是单块"（`scf.reduce`）。
4. **软关键字让领域名不被语言实现绑架。** 属性叫 `op` 是领域事实，
   语言应该适配，而不是要求领域改名。
5. **直觉会被 ODS 打脸。** `gpu.launch`/`gpu.func`/`func.func` 都不是
   sized region；"函数体当然单块"是 IR 层的想象，不是 ODS 的声明。
