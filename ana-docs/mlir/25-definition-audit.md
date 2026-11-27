# 25. 定义层契约：范围、形状、证据、预算

> 配套：`tools/def-audit.py`（D0–D14，`--selftest` 10 例，失败退出 1）、
> `tools/def-exceptions.tsv`（7 列，34 行）、文法 `dialect-grammar.y`
> （`Assembly Modeled;`、`params`、`struct(params)`）。
> 上位规则见 [24 章](24-escape-licenses.md)：E0–E10、S1–S10、L0–L12。
> 语料：50 个方言、276 个定义（100 type / 35 attr / 141 enum）。

## 0. 问题：逃逸点不是黑洞

24 章把 op 层的每个逃逸都写成"带证据的断言"。定义层当时没有审计器：
`attr_def` / `type_def` 可以随便挑一个通道、丢掉一个参数、引用一个不存在的
符号，没有任何东西会发现。更糟的是上一版 `def-exceptions.tsv` 的头注释写着：

> modeled declarations ... may also carry a block, which is then checked for
> internal coverage only.

这句话本身就是一个黑洞的许可证：一旦某条记录没有上游 ODS 记录，块里写什么
都只做"内部自洽"检查。于是逃逸点从"我不知道上游怎么打印"退化成"我想怎么写
就怎么写"。这正是本轮要消灭的东西。

把定义层的每条逃逸都按四个问题收紧（§1）之后，第一次运行就抓到 6 处已有的
滥用——不是新写的，是之前没人看的：

| 位置 | 旧写法 | 违反 | 处置 |
| --- | --- | --- | --- |
| `llvm.metadata` | `Assembly Generic;` | D13：没有上游记录，Generic 不可证伪 | `Assembly Modeled;` |
| `tosa.DenseElements` | `Assembly Custom;` | D13：走的是内建 `dense<...>` 打印器，不是 Tosa 的格式 | `Assembly Modeled;` |
| `tosa.IndexElements` | `Assembly Custom;` | 同上（`array<i64: ...>`） | `Assembly Modeled;` |
| `quant.Uniform` | 带 `if`/`oilist` 的块 | D11：modeled 块只能是扁平脊 | `Modeled;` + Doc 记录两种拼写 |
| `quant.Any` | 带 `if` 的块 | D11：同上 | `Modeled;` + Doc |
| `sparse_tensor.level_type` | 带 `if` 的块 | D11：同上 | `Modeled;` + Doc |

注意处置方向：**不是把规则放宽到能接受这些块，而是把块删掉**，把信息移进
`Doc` 和注释。理由：没有上游 `assemblyFormat` 的类型，文本由 C++ printer 决定；
审计器读不到那个 printer，任何块都只是"看起来可审计"。诚实的写法是承认它是
黑盒（`Modeled`），而不是写一个假的格式。这就是"dsl 不解决一切"的具体形态。

## 1. 每个逃逸点必须回答四个问题

| 约束 | 问题 | 定义层的落点 |
| --- | --- | --- |
| **范围** scope | 它能出现在哪 | 只在 `attr_def` / `type_def` / `enum` 体里；不进 op（L12） |
| **形状** shape | 它允许什么结构 | `Modeled` = 不声称文本；spine = 字符串 + 已声明参数；`Custom`/`Generic` 只能是对**真记录**的断言 |
| **证据** witness | 凭什么说它成立 | 上游记录（D1–D3）或 ledger 行 + 符号级见证（D8） |
| **预算** budget | 一个方言能藏多少 | modeled ≤ 12，其中带参数 ≤ 4（D12） |

四条缺一，这个通道就不该存在于文法里。下面每个 D 规则都能指到一个检测器；
没有检测器的规则不写进来（与 24 章同一条纪律）。

## 2. 定义层的四个通道

| 通道 | 准入（证据） | 它声称什么 | 检测 |
| --- | --- | --- | --- |
| `Assembly { ... }` | 上游记录有非空 `assemblyFormat` | 本文法自己的描述 | D2 + D4 + D7 + D14 |
| `Assembly Custom;` | 上游 `hasCustomAssemblyFormat = 1`，且没有 `assemblyFormat` | 上游把文本交给手写打印器 | D3 |
| `Assembly Generic;` | 上游既无格式也无手写打印器 | 上游没有自定义文本，只有固定拼写 | D3 |
| `Assembly Modeled;` | ledger 行（`upstream = -`） | **不声称**文本形状；文本在 C++ 里 | D9 + D13 |
| 扁平脊（modeled + 块） | 同上，且块只由字面量/参数组成 | 记录了 C++ 打印器的**骨架**，不是它的控制流 | D11 + D4 |

第五行不是第五个通道，而是 `Modeled` 的可选形态：一个 modeled 定义要么完全
不声称文本（`Modeled;`），要么只声称"这些参数和标点按顺序出现"（spine）。
中间地带——带 `if`/`oilist`/钩子的块——不存在。

当前 34 行 ledger 的分类：

| category | 行数 | 例子 |
| --- | --- | --- |
| `cxx-type` | 20 | `gpu.mma_matrix`、`quant.Uniform`、`spirv.image` |
| `borrowed-enum` | 4 | memref/xegpu 借 arith 的 `AtomicRMWKind` |
| `builtin` | 3 | tosa 的 `DenseElements`/`IndexElements`/`SymbolName` |
| `rename` | 3 | transform 的 `operation`→`op`、`type_param`→`type` |
| `cxx-enum` | 2 | sparse_tensor 的 `LevelFormat`/`LevelProperty` |
| `constraint` | 2 | tosa `AccumulatorType`（`TypeAttrOf<Tosa_AccType>`）、toy `struct` |

## 3. 规则 D0–D14

| # | 规则 | 为什么 | 检测 |
| --- | --- | --- | --- |
| D0 | **命名必须解析**：每个声明要么命中上游记录，要么有 ledger 行 | 拼错的声明会静默变成"本文法自创" | `audit()` 的 `D0 naming` |
| D1 | **通道必须显式**：每个定义都写 `Assembly` 子句 | 省略不等于 Generic；省略是看不见的黑洞 | `D1 channel` |
| D2 | **块 ⇔ 上游有声明式格式** | 块是描述，不是"我想写一个格式"的许可 | `D2 block` |
| D3 | **Custom/Generic ⇔ 上游的手写/无格式** | 断言必须可证伪 | `D3 custom` / `D3 generic` |
| D4 | **块里每个标识符都是已声明参数，每个参数至少出现一次** | 丢参数和丢 operand 是同一种洞 | `block_refs` |
| D5 | **枚举拼写与数值逐项一致**；group 按值（成员 OR）核对 | 枚举是文本契约的一部分 | `D5 cases` |
| D6 | **`: bits` ⇔ 上游是 bit-enum** | 位宽语义不能靠猜 | `D6 kind` |
| D7 | **`params`/`struct(params)` 是全量指令**：体内至多一次、不与具名参数混用、不进钩子/oilist 实参、必须与上游格式一致 | "全部参数"不能变成"我想打印的那些" | `D7 family` |
| D8 | **证据必须解析**：alias 目标存在；modeled 见证文件存在，且**引用行确实提到 reason 里反引号的符号** | 只指到文件不是证据，指到符号才是 | `witness_problem()`，`def-audit.py:408` |
| D9 | **`Modeled;` 必须有 ledger 行**；有上游记录的声明不得使用 | 逃逸不能顺手用在可查证的地方 | `D9 modeled` |
| D11 | **modeled 块是扁平脊**：只允许字符串、已声明参数、`params`/`struct(params)` | 没有上游格式的块若带控制流，就是一个私人文法 | `spine_tokens()`，`def-audit.py:445` |
| D12 | **modeled 预算**：每方言 ≤ 12；其中声明参数的 ≤ 4 | 无上游证据的结构不能无限堆积 | `D12 budget` |
| D13 | **modeled 只能 `Modeled;` 或脊**，不得 `Custom`/`Generic` | 没有上游记录时，这两个词无法被证伪 | `D13 channel` |
| D14 | **定义块里的 `custom<Hook>` 必须出现在该记录的上游 `assemblyFormat` 里** | 类型里的钩子也是钩子，需要和 op 层 L3 同等的证据 | `D14 hook` |

D8 的符号规则值得单独说：上一版只检查 `witness` 的文件存在，于是一个行号可以
指向任何文件。现在按类别要求 `path:line`，并要求该行（±3 行窗口）出现
reason 中第一个反引号符号。模板应用取头部（`` `TypeAttrOf<Tosa_AccType>` ``
检查 `TypeAttrOf`）。34 行全部通过；规则本身由 `--selftest` 的 5 个拒绝路径钉住。

## 4. category 是封闭词表，不是自由标签

`category` 列不是注释，它决定见证必须长什么样。审计器对每一类做交叉检查：

| category | kind 限制 | witness 形状 | 额外规则 |
| --- | --- | --- | --- |
| `rename` | type/attr/enum | `-` | 必须有 upstream，且目标存在（D8 stale） |
| `cxx-type` | type/attr | `.h` 或 `.td` | 符号必须在见证行 |
| `cxx-enum` | enum | `.h` | 同上 |
| `builtin` | type/attr | 必须在 `mlir/include/mlir/IR/` 下 | 内建属性/类型本来就没有方言记录 |
| `constraint` | type/attr | `.td` | 约束别名（`TypeAttrOf<...>`），不是 `def` |
| `borrowed-enum` | enum | 不在本方言自己的目录下 | 借用别的方言的枚举记录 |

新增一类需要同时改 `CATEGORIES`、本表、`witness_problem()` 和 `--selftest`。
"其他"不是一个类别；如果一条记录找不到归类，它就不该是 modeled。

## 5. 示例：一个合法的脊

`spirv.tensor_arm` 没有 ODS TypeDef，但它的参数和标点是确定的，所以它用脊
而不是 `Modeled;`：

```text
type_def tensor_arm {
  Doc "...";
  Parameters (element: any, rank: integer);
  Assembly { "<" element "," rank ">" }
}
```

三个元素都是字面量或已声明参数，`spine_tokens()` 返回空集。若把它写成
`"<" element if rank { "," rank } ">"`，D11 会报 `if`；写成
`custom<Foo>(element)` 会报 `custom`；写成 `type(element)` 会报 `type`。
脊表达的是"顺序与标点"，不是"什么时候打印"。

反过来，`quant.Uniform` 的真实文本有两种形态（per-layer / per-axis），必须
用 `if` 和 `oilist` 才能表达。这正说明它**不是**一个脊，因此它只能是
`Modeled;`，把两种拼写写进 `Doc` 作为人类可读的证据。

## 6. 覆盖模式：绿灯不等于完整

`def-audit.py` 默认只证明"声明的东西是真的"，不证明"上游的东西都声明了"。
后者是 `--coverage` 的职责，故意分开：

| 模式 | 断言 | 当前 |
| --- | --- | --- |
| 默认 | 每条声明都有证据（D0–D14） | 276 条，0 problems |
| `--coverage` | 上游的每个定义都被声明 | 缺 381 条 |
| `--selftest` | 规则本身能拒绝坏输入 | 10 例，0 failed |

覆盖表自己也修了两个"反黑洞"：
- `extra` 只列"既没有上游记录、也没有 ledger 行"的声明。之前 modeled 行会被
  算成 extra，真正的伪造声明可以混在里面；现在 ledger 行被扣除，`extra`
  必须为空（当前 50/50 全空）。
- `missing` 扣除 alias 的目标。transform 的 `op`/`type`/`affine_map` 是改名
  声明，不是遗漏；不扣就会永久欠着三条假账。

缺口的分布（按方言，数量从少到多）给出下一轮的顺序：

| 方言 | 缺 | 方言 | 缺 |
| --- | --- | --- | --- |
| arith / arm_sme / complex / mpi / shard / transform | 各 1 | llvm | 100 |
| amdgpu / nvgpu | 各 2 | nvvm | 106 |
| irdl | 3 | spirv | 55 |
| tosa | 4 | rocdl | 29 |
| ptr / xegpu / xevm | 各 5 | acc | 20 |
| linalg | 8 | gpu | 18 |
| omp | 10 | 合计 | 381 |

策略与 op 层一致：先小后大，每批补完跑全套四个审计器；枚举用上游 `Cases`
逐项生成，`struct(params)` 形态直接写成 `<" struct(params) ">` 由 D7 验证。

## 7. 与 24 章的分工，以及本轮新增的三条 op 层约束

24 章管 op 的文本通道，本章管定义。跨层的三条新规则放在 escape-audit：

| 规则 | 内容 | 为什么 |
| --- | --- | --- |
| **L3d** | 本地钩子标签必须由它代表的子句关键字字面量引出（`"private" "(" custom<PrivateClause>(...)`），关键字由标签名去掉 `Clause`/`Op` 得到 | 否则标签就是一个没有范围的别名 |
| **L4b** | gap 的 `scope` 是封闭词表（`text-spine` / `optional-group` / `hook-args` / `oilist`），不在表内即失败 | "缺口在别处"不是许可证 |
| **L12b** | `Modeled` 只属于定义层；op 用它直接失败 | op 总有上游记录可用来命名通道 |

L3d 同时补了一个反向检查：登记了但没人用的标签要退役（`L3` stale）。
当前三个 omp 标签全部通过，因为它们分别被 `"private"`、`"reduction"`、
`"task_reduction"` 字面量引出。

## 8. 结论

1. **逃逸点的最小完备描述是四元组**：范围、形状、证据、预算。少一项就会
   长出黑洞——上一版缺的是"形状"和"证据的粒度"。
2. **没有上游格式的类型不该有块。** 块看起来可审计，实际无法对照任何东西；
   `Modeled;` + Doc 是更诚实的表达。第一次运行删掉的三个块都是这个逻辑。
3. **category 把"为什么"变成可检查的结构。** 自由散文的 reason 只能说服人，
   封闭词表的 category 能说服审计器：`cxx-type` 的见证必须是头文件里的符号，
   `builtin` 的见证必须在 `mlir/IR/`。
4. **预算是范围的一部分。** 12/4 两个数字不代表美学，只代表"一个方言最多能
   把多少结构藏进无证据通道"；超过它，应该去补声明，而不是加 ledger 行。
5. **覆盖与健全性必须分开。** 绿灯只说明"说出来的都是真的"；`--coverage`
   才回答"是不是都说出来了"。把两者混在一个数字里，是另一种黑洞。
