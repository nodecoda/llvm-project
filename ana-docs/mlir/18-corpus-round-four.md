# 18. 第四轮语料：pdl_interp / tosa / shape / async / quant

> 配套文件：[`corpus/pdl_interp.dialect`](corpus/pdl_interp.dialect)、
> [`corpus/tosa.dialect`](corpus/tosa.dialect)、
> [`corpus/shape.dialect`](corpus/shape.dialect)、
> [`corpus/async.dialect`](corpus/async.dialect)、
> [`corpus/quant.dialect`](corpus/quant.dialect)
> （合计 1864 行、147 个 op）；
> [`dialect-grammar.y`](dialect-grammar.y)（**58 条规则 / 136 条产生式**，0 冲突）；
> 19 个语料文件全部 `parse OK`。
>
> 与 17 章的关系：17 章证明"新方言不必新增产生式"；本章换了检验目标——
> 当 ODS **自己都装不下**某个领域概念时，它落在哪里，我们的语言该不该接。

## 0. 这一轮换了检验目标

前三轮的问题是"我们的文法够不够"。这一轮的问题更尖锐：**ODS 装不下的东西，
落在 C++ 里，那它还是不是领域事实**？五个方言分别提供了五种落点：

| 方言 | ODS 的落点 | 本轮的判决 |
| --- | --- | --- |
| `quant` | 类型文法整个手写在 `TypeParser.cpp` | **是领域事实**，用 `type_def` + `Assembly` 收回 |
| `pdl_interp` | `MinSizedRegion<1>` 只是约束类 | **语料没用到新词**，region 事实本来就装得下 |
| `tosa` | `DenseArrayCount<2..6>` 只是约束类 | **秩与长度是一等事实**，进类型实参与绑定注解 |
| `shape` | 接口方法覆写、generic 格式 | 一半是领域事实，一半不是 |
| `async` | 类型名 `Token` 与拼写 `token` 分离 | **声明名就该是拼写**，ODS 的类名是 codegen |

## 1. 语料

| 文件 | 行数 | dialect | constraint | enum | attr_def | type_def | schema | op |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `pdl_interp.dialect` | 453 | 1 | 5 | | | | 4 | 39 |
| `tosa.dialect` | 648 | 1 | 5 | 9 | 9 | 1 | 21 | 36 |
| `shape.dialect` | 402 | 1 | 6 | | | 4 | 6 | 40 |
| `async.dialect` | 280 | 1 | 2 | | | 6 | 3 | 29 |
| `quant.dialect` | 81 | 1 | 4 | | | 4 | 1 | 3 |
| 其余 14 个方言 | 1809 | 14 | 8 | 14 | 1 | 4 | 19 | 199 |
| **合计** | **3673** | **19** | **30** | **23** | **10** | **19** | **54** | **346** |

> 本表在第五轮复核后修正（初版为 3667 行、29 constraint、20 type_def）：
> 事实核对改了 async/tosa/shape/pdl_interp 的几行，行数与计数随之变化。

各方言的领域贡献：

- **pdl_interp**：把"匹配"展开成基本块原语。谓词原语把"判定"和"二路分支"
  融进一个 op，`switch` 把"取值"和"多路分派"融合；39 个 op 里有 20 个是
  `get_*`/`check_*` 的同形族，由 4 个 schema 覆盖。
- **tosa**：属性层最厚。定长整数数组、枚举、属性包（`TargetEnv`）、符号名
  属性，以及"同一件事的两种形状"——`avg_pool2d` 的窗口是属性，
  `avg_pool2d_adaptive` 的是 rank-2/rank-4 的 shape 操作数。
- **shape**：只有类型层，没有数据。它引入 `witness` 这种"只存在于前端、
  不产生运行时数据"的值，以及 `assuming` 的单块前提 region。
- **async**：六个自有类型（`token`/`group`/`value`/`coro.id`/`coro.handle`/
  `coro.state`）与"值既是结果又是 region 入口"的执行体。
- **quant**：三个 cast，四个类型——而四个类型的文法**一行都不在 TD 里**。

## 2. 文法增量：6 条产生式，其中 1 项零成本

| # | 构造 | 见证 | 动作 |
| --- | --- | --- | --- |
| 1 | custom 实参是派生类型 `type(x)` | `PDLInterpOps.td:457`、`AsyncOps.td:353` | `custom_arg` 加一支 |
| 2 | `successors` 打印全部后继 | `PDLInterpOps.td:117` 起 8 处 | 新 token + 1 条产生式 |
| 3 | 属性值域 `[positive]`/`[non_negative]` | `AsyncOps.td:708`、`PDLInterpOps.td:250` | binding 注解（2 条产生式） |
| 4 | 类型实例的整数值实参 `shape<4>` | `TosaTypesBase.td:374`、`TosaOps.td:2124` | `type_arg` 加一支（2 条产生式） |
| 5 | 最小块数 `[min_blocks(1)]` | `PDLInterpOps.td:672` | **零改动** |

第 5 项值得单说：16 章把 region 约束改成"注解列表"，注解元素就是**事实**。
事实本来就能带整数实参，所以 `MinSizedRegion<1>` 一行文法都不用改：

```text
Regions (body: region [min_blocks(1)]);
```

设计先行时不会想到要"恰好一块"和"至少一块"两种说法；但把约束统一成事实之后，
新的一种只是新的事实名，不是新的语法。

### 2.1 `successors` 与 `type(x)` 的形状

```text
Assembly { "of" input_op "is" count attr-dict "->" successors }
Assembly { custom<CreateOperationOpResults>(input_result_types,
                                            type(input_result_types),
                                            inferred_result_types) }
```

`successors` 是**格式指令**而不是部件：它打印的是"所有后继"，没有名字可引用。
这正是 13 章区分"部件"与"指令"的地方——`attr-dict`、`type(x)`、
`functional-type(a,b)`、`oilist(...)`、`successors` 都是指令。

### 2.2 属性值域为什么挂在绑定上

```text
Attributes (count: i64 [positive]);
Attributes (count: i32 [non_negative], compare_at_least: bool = false);
```

`positive` 不是类型的一部分：同是 `i64`，一个属性要求正数，另一个不要求。
它是**这个绑定的取值域**。把注解放在绑定上，和 16 章把块数约束放在 region
绑定上是同一个动作——约束挂在它实际约束的那个东西上。

## 3. 五个判决

### 3.1 quant：ODS 装不下时，落点在哪

`!quant.uniform<u8:f32, 2.0:128>` 的解析是 `TypeParser.cpp:544` 里手写的
`if (typeNameSpelling == "uniform")` 分派，该文件用注释给出了完整 BNF：

```text
uniform_per_layer  ::= `uniform<` storage-spec expressed-type-spec
                       `,` scale-zero `>`
uniform_per_axis   ::= `uniform<` storage-spec expressed-type-spec
                       axis-spec `,` `{` scale-zero-list `}` `>`
```

三种形状共用一个助记符，靠标点区分。用领域语言写出来是：

```text
type_def uniform {
  Parameters (storage: storage, expressed: float_like, axis: optional<i64>,
              scales: array<f64>, zero_points: array<i64>);
  Assembly { "<" storage ":" expressed if axis { ":" axis }
             "," oilist("{" scales "}:{" zero_points "}" | scales ":" zero_points)
             ">" }
}
```

`oilist` 在这里正好是"至多一种形状"。**这是 `type_def` + `Assembly` 的合法性
证明**：ODS 表达不了"类型自身的文本形状"，只能落到 C++；我们的语言能表达。

### 3.2 不是缺口：`type_def` 的名字

async 在 ODS 里叫 `Async_TokenType`，拼写是 `token`。语料一度照抄成
`type_def Token`，再用 `constraint token = Token` 拉回来——这是一层纯噪音。
声明名就该是**打印拼写**，C++ 类名不是领域事实：

```text
type_def token { Doc "becomes available when its producer finishes; carries nothing"; }
type_def value { Parameters (value_type: any); Assembly { "<" value_type ">" } }
```

`pdl.dialect` 从一开始就是这么写的（`type_def attribute`/`operation`/`range`）。
`quant` 的四个类型同理：`uniform`、`any`、`calibrated`、`quantile`。

### 3.3 不是缺口：trait 的列表实参

`AllShapesMatch<["values", "output"]>` 看着像"trait 带列表实参"，但事实本来
就是变长的：`all_shapes_match(values, output, input)` 用现有的
`fact_arg_list` 就够。ODS 的方括号是容器语法，不是领域结构。

### 3.4 是缺口，但不该进语言：region 入口块参数

`async.execute` 的 region 入口块参数由操作数解包而来，`pdl_interp.foreach`
的入口块参数类型等于输入 range 的元素类型。ODS 也没有对应声明——它是
**verifier 的约束**，不是定义的结构。语料用 region 事实说这件事：

```text
Regions (body: region [single_block,
                       implicit_terminator(yield),
                       entry_args_unwrap(body_operands),
                       yield_types_match(body_results)]);
```

判决：**定义语言只描述"结构 + 文本 + 可陈述的事实"**；入口块参数的
一致性是"事实"（`entry_args_unwrap`），不是"语法位置"。

### 3.5 是缺口且推迟

| 缺口 | 见证 | 为什么推迟 |
| --- | --- | --- |
| `ref(type($x))` | `PDLInterpOps.td:1043` | 只有 1 处；`ref` 是消歧提示，语义待定 |
| `type_def` 的底层类型 | `ShapeBase.td:94` 的 `extent_tensor` | 需要"等价于 rank-1 index 张量"这种等式语法，值得单独设计 |
| 接口方法覆写 | `ShapeOps.td:130` 的 `["getAsmResultNames"]` | 覆写列表是实现细节，边界未定 |
| 匿名位置部件 | `AsyncOps.td:223` 的无名结果 | 名字在本语言里是"供 Assembly 引用"的装置，给无名结果起个名是可接受代价 |
| generic 格式 | `ShapeOps.td:691`、`:703` | 一度按“缺省即通用”处理；19 章 R1 推翻了这一点：省略是看不见的逃逸点，改为显式 `Assembly Generic;` |

## 4. 属性层：cardinality 挂在哪一层

tosa 的定长数组提供了两种相反的见证：

```text
/* 一个 schema 的所有实例都同长 → 挂绑定 */
Attributes (kernel: array<i64> [cardinality(2)], pad: array<i64> [cardinality(4)]);

/* 同一 schema 的不同实例长度不同（conv2d 4/2/2、conv3d 6/3/3）→ 挂 op 事实 */
op conv2d from Conv { Properties [cardinality(pad, 4), cardinality(stride, 2), ...]; }
```

判据是**约束的作用范围**，不是"哪种写法更短"：绑定的注解是这个绑定的性质；
op 的 Properties 是这个实例的性质。17 章的 `affine.if`（then 一块、else 不限）
是同一个判据的另一个面。

## 5. 秩进入类型

`Tosa_Tensor4D` 是"秩为 4 的张量"，`Rank2TosaShape` 是"秩为 2 的 shape"。
写进语料是：

```text
Operands (input1: tensor<number, 4>, kernel: shape<2>, pad: shape<4>);
```

这是 17 章 `custom_arg : A_TYPE L_PAREN IDENT R_PAREN` 的同构结论在类型层的
对应：**类型应用的实参可以是值**（秩、长度），不必另造一个类型名。
`constraint int2 = array<i64>;` 那种"用名字编码长度"的写法随之删除。

## 6. 机械核对

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
| 语料解析 | 19/19 `parse OK` |
| `from`/`extends` 指向不存在的 schema | 0 |
| Assembly 引用未声明部件 | 0 |
| 声明后未使用的 schema/constraint/enum | 0 |
| C++ 泄漏（`::`、`namespace`、`#include`、`$x`、`[{`） | 0 |

统计（19 个文件）：

```text
assembly: attr-dict=214, type=206, if=80, operands=76, results=38,
          functional-type=32, custom=29, oilist=12, successors=8
region facts: single_block=33, min_blocks=1, implicit_terminator=1,
              entry_args_unwrap=1, yield_types_match=1
type ctors: variadic=153, optional=103, array=81
```

## 7. 局限与下一轮

1. **`ref(...)` 与类型等式**仍未表达（§3.5）。
2. **匿名位置部件**用命名代替（`AsyncOps.td:223`）。
3. **llvmir / spirv** 这两个体量最大、类型系统最异构的方言还没碰
   （`SPIRV` 432 个 def），它们会检验"枚举与类型参数爆炸"时的可读性。
4. **`emitc` / `ptr` / `ub`** 只有个位数 op，适合验证"小方言是否也需要
   schema"。
5. 剩余方言里大量是同一形状的重复（逐元素、规约、内存访问），下一轮起
   应优先挑**形状不同**的，而不是 op 多的。

下一轮语料：`sparse_tensor`（编码属性）、`bufferization`（7 个 op 但语义重）、
`llvmir`（类型异构）、`emitc` + `ub`（小方言）、`ptr`（指针类型系统）。

## 8. 结论

1. **"ODS 装不下"不等于"不是领域事实"。** quant 的四个类型文法全在
   `TypeParser.cpp`；它仍然是最纯粹的领域事实——存储/表达/比例/零点。
   `type_def` + `Assembly` 的存在理由就是这一类。
2. **很多"缺口"是我们用错了词。** `type_def Token` 是照抄 C++ 类名；
   `int2 = array<i64>` 是把长度编码进名字；`all_shapes_match([a, b])` 是照抄
   容器的方括号。换成领域词，缺口就消失了。
3. **统一成"事实"的收益是复利。** region 注解在 16 章变成事实列表，
   17 章接住了 per-region 约束，18 章零成本接住 `min_blocks`。
4. **约束挂载点是设计问题，不是风格问题。** 绑定注解表达"这个部件的性质"，
   op 的 Properties 表达"这个实例的性质"；两者不可互换。
5. **值可以进类型。** 秩与长度是值，`tensor<number, 4>`、`shape<2>` 比
   `Tosa_Tensor4D`、`Rank2TosaShape` 更直接。
