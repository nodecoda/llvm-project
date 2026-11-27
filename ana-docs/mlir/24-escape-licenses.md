# 24. 逃逸点许可证：证据、范围、义务、代价

> 配套：`tools/escape-audit.py`（L0–L6，失败退出 1）、
> `tools/gap-registry.tsv`、`tools/fact-glossary.tsv`、`tools/hook-registry.tsv`；
> 文法：`dialect-grammar.y` 的 `Gap(name)`；上位规则见 [19 章](19-escape-hatches.md)。
> 语料：39 个方言、1171 个 op（本章数字随语料更新；历史数字见各轮章节）。

## 0. 19 章留下了什么口子

19 章规定了"哪些通道、什么时候用、怎么计数"，但它只检查了**有没有写**，
没有检查**写的内容和上游是否一致**。第一次跑证据对照，31 个方言里就有
16 个 op 把明明是声明式的文本标成 `Assembly Custom;`：

| 方言 | op | 上游真实形态 |
| --- | --- | --- |
| cf | `br`、`cond_br` | 纯声明式 `assemblyFormat` |
| cf | `switch` | 声明式 + `custom<SwitchOpCases>` |
| func | `call`、`call_indirect` | 声明式（含 `prop-dict` / `oilist<,>` 分隔符） |
| emitc | `file` | `$id attr-dict-with-keyword $region` |
| emitc | `call_opaque`、`member_call_opaque` | 声明式（含 `prop-dict`） |
| emitc | `switch` | 声明式 + `custom<SwitchCases>` |
| amdgpu | `packed_trunc_2xfp8` 等 3 个 | 声明式（含 `($x^):("undef")?`） |
| scf | `reduce`、`reduce.return` | 纯声明式 |
| scf | `index_switch` | 声明式 + `custom<SwitchCases>` |
| pdl_interp | `create_range` | 声明式 + `custom<RangeType>(ref(...))` |

`Custom` 的语义是"上游把文本交给手写打印器"。这 16 个 op 的上游没有手写
打印器，所以这 16 处不是措辞问题，是**假陈述**。逃逸点一旦可以假陈述，
它就重新变成了黑洞。

## 1. 断言与描述

这一章只引入一个区分：

| 类别 | 记号 | 它说了什么 | 证据 |
| --- | --- | --- | --- |
| **断言** | `Custom` / `Generic` / `custom<Hook>` / `Gap(name)` | 关于**上游**或**本文法**的事实 | 必须能被 TD 或登记表核对 |
| **描述** | `Assembly { ... }` | 本文法自己定义的文本形态 | 只需内部自洽（引用可解析） |

`Custom` 说"上游有手写打印器"，`Generic` 说"上游没有自定义格式"，
`Gap(p)` 说"上游有声明式格式，但本文法缺 `p` 这个构造"。三者都是断言，
都能被证伪，因此都必须带证据。

每个通道因此有四元组：

```text
许可证 = <准入 evidence, 范围 scope, 义务 obligation, 检测 detector>
```

## 2. 五通道许可证

| 通道 | 准入（evidence） | 范围（只覆盖） | 义务 | 检测 |
| --- | --- | --- | --- | --- |
| block | 无（描述） | 已声明部件、字面量、`type`/`attr-dict`/`oilist` 等指令 | 引用必须可解析 | L0 + 悬挂引用 |
| `Custom` | `hasCustomAssemblyFormat = 1`（含继承，`1` 或 `true`） | 该 op 的**全部**文本 | 结构仍在子句里声明 | L1 |
| `Generic` | 既无 `assemblyFormat` 也无手写打印器 | 无自定义文本 | 结构仍要声明 | L2 |
| `custom<Hook>(a,b)` | 钩子名在上游 TD 闭包里出现，或在 `hook-registry` 登记 | 只覆盖实参列出的部件 | 实参必须已声明 | L3 |
| `Gap(name)` | 该名字在 `gap-registry` 登记，见证集合精确 | **只表示缺失的那一处**；块其余部分照常描述 | 一个 op 至多一个，写在块首位 | L4 |

两条容易混的边界：

- `custom<Hook>` 的**名字**是本文本的标签，不要求与上游同名；但若上游有同名
  `custom<>` 就优先用同名，找不到对应物时必须在 `hook-registry` 解释。
  这条检查抓到过真错误：`emitc.global` 把上游
  `custom<EmitCGlobalOpTypeAndInitialValue>` 写成了
  `custom<GlobalTypeAndInitialValue>`——语法合法、名字悬空。
- `Gap(name)` 不是第六种逃逸。它不提供"随便写"的自由：块还是要写，
  只是允许在**一个具名位置**承认缺口。缺口有名字、有见证、有预算。

## 3. 范围细则（scope）

通道的自由度由"它能提到什么"决定。四条硬边界：

| # | 规则 | 违反的样子 | 检查 |
| --- | --- | --- | --- |
| S1 | 块里的标识符必须是本 op/schema 链上已声明的部件或指令 | 写了一个没声明的名字 | 悬挂引用 |
| S2 | `custom<Hook>(...)` 的实参只能是已声明部件，或 `type(x)` 派生 | 钩子顺手引入新部件 | 悬挂引用 + L3 |
| S3 | `Gap(name)` 只能出现一次且位于块首位；块其余部分必须能独立解析 | 用 Gap 把整段文本吞掉 | L4（见 `escape-audit.py` 的 gap 提取） |
| S4 | 事实只能描述已声明部件；本地事实只属于一个方言 | 借别的方言的词 | L5 |

S3 是这一轮新增的：`Gap(ref_type) { ... }` 的块必须仍然可解析、引用可解析，
所以"缺口"不能被用来隐藏结构。`Assembly Custom;` 能隐藏整段文本，`Gap` 不能。

## 4. 检测器（L0–L6）

`tools/escape-audit.py` 对每个方言做三件事：从语料取通道，从上游 TD 闭包
（跟随 `include`）取证据，再逐条比对。

| 规则 | 判据 | 失败含义 |
| --- | --- | --- |
| L0 | 每个 op 必须有文本通道（直接写或从 schema 继承） | 有 op 没交代文本 |
| L1 | `Custom` ⇒ 上游有手写打印器 | 假 Custom：把可描述的文本推给打印机 |
| L2 | `Generic` ⇒ 上游两种格式都没有 | 假 Generic：把有格式的 op 说成没有 |
| L3 | 钩子名 ∈ 上游 `custom<>` ∪ `hook-registry` | 拼错或随手造钩子名 |
| L4 | `Gap(name)` ∈ `gap-registry`，且见证集合与语料完全一致 | 未登记缺口 / 登记表过期 |
| L5 | 事实 ∈ `fact-glossary`；本地事实不越界 | 新词绕过词表 |
| L6 | 预算（§6） | 结构性问题，见下 |

L1/L2 的判据来自上游，不来自语料，所以它们不会"自证"。这也解释了为什么
16 处假 Custom 能活到现在：19 章的检查只看语料内部。

## 5. 缺口机制：把"说不出"变成债务

`Gap(name)` 只解决一件事：**上游是声明式的，本文法还没有对应产生式**。
它把这种状态从"偷偷用 Custom"变成一条有名字的债务。

`tools/gap-registry.tsv` 四列：`gap`、`witness`（哪些 op）、`missing`（缺什么）、
`status`。本轮关掉一条（`prop_dict`），当前 3 条、6 个 op：

| gap | 见证 | 缺的构造 | 关闭方式 |
| --- | --- | --- | --- |
| `optional_default` | amdgpu 3 个 packed op | `($x^):("undef")?`：缺省时打印固定字面量 | 给 `if` 加 `else "literal"` |
| `ref_type` | `cf.switch`、`pdl_interp.create_range` | `ref(type($x))` 作为钩子实参 | 加 `ref(type(x))` 产生式 |
| `oilist_separator` | `func.call_indirect` | `oilist<sep>(...)` 的分隔符 | 给 `oilist` 加首参 |

三条都是**由真实 op 逼出来的**，符合"先写源程序，文法跟不上才是新产生式的
合法来源"。L4 保证缺口不会被悄悄扩大：新增一个 `Gap(foo)` 而没登记，
工具直接失败；登记表里的见证和语料不一致，也失败。

关闭一条缺口就走同一条路：新语料的 XeGPU/NVGPU/XeVM 共 23 个 op 的格式里
有 `prop-dict`，它的位置逐 op 不同（都在操作数之后、`attr-dict` 之前），
证据足够，于是给文法加 `A_PROP_DICT` 产生式，把三条 `Gap(prop_dict)` 改写成
`prop-dict`，再删登记表里的那一行。缺口不是被预算挤掉的，是被语料补上的；
预算 6 / 12 让剩下的三条继续有界。

预算把缺口限制在可关闭的规模：`named gaps <= 12`，当前 9。

## 6. 预算与现状

| 指标 | 当前 | 红线 | 说明 |
| --- | --- | --- | --- |
| 无文本通道的 op | 0 | 0 | 每个 op 都交代了文本来源 |
| `Custom` 占 op 比 | 67/1171 = 5.7% | 25% | 过高说明块格式表达力不足 |
| 具名缺口 | 6 | 12 | 每个缺口都有登记与见证 |
| 单次使用的本地事实 | 7/20 = 35% | 50% | 事实不能退化成注释 |
| 设计分歧（block vs 上游 custom/generic） | 22 | 30 | 见 §7 |

`Custom` 从 19 章的 75/692 = 11% 降到 5.7%，不是删了 op，而是 16 处假 Custom
被拆成 7 个块格式和 9 个具名缺口；随后 `prop_dict` 那 9 个见证里 3 个改写成
新产生式，另外 20 个新 op 直接用了 `prop-dict`，缺口总数降到 6。

## 7. 设计分歧不是逃逸

还有 22 个 op 的块格式与上游"不一致"：

| 上游形态 | 数量 | 例子 | 性质 |
| --- | --- | --- | --- |
| 手写打印器 | 12 | `linalg.matmul`、`arith.select`、`emitc.include` | 本文法用统一 schema 把上游逐 op 手写的文本**重建**出来 |
| 无任何格式 | 10 | `arm_sve.sdot`、`arm_sme.zero` | 本文法为上游的 generic 形态**定义**了一个可读文本 |

这两类都不是逃逸：没有跳过任何结构，块里的每个引用都能解析。它们是
**描述**层面的设计选择，所以 L1/L2 不管，只计数并设上限（30）。
超过上限时先问："是不是块格式缺了某个构造，才逼得语料去猜上游？"

## 8. 本轮处置明细

| 原状态 | 处置 | 数量 | op |
| --- | --- | --- | --- |
| 假 `Custom` → 块 | 按上游格式重写（块或块+钩子） | 7 | `cf.br`、`cf.cond_br`、`emitc.file`、`emitc.switch`、`scf.reduce`、`scf.reduce.return`、`scf.index_switch` |
| 假 `Custom` → 具名缺口 | 块 + `Gap(name)` | 9 | cf/emitc/func/amdgpu/pdl_interp 见 §5 |
| 钩子名悬空 | 改名 | 1 | `emitc.global`：`GlobalTypeAndInitialValue` → `EmitCGlobalOpTypeAndInitialValue` |
| 钩子名为本地标签 | 登记 | 3 | omp 的 `PrivateClause`、`ReductionClause`、`TaskReductionClause` |

重写后的块格式不是抄 TD：它们用本文法的 `if`、`oilist`、`custom<>`、
`functional-type` 表达同一结构。以 `cf.cond_br` 为例：

```text
Assembly { condition if branch_weights { "weights" "(" branch_weights ")" } ","
           true_dest if true_dest_operands { "(" true_dest_operands ":" type(true_dest_operands) ")" } ","
           false_dest if false_dest_operands { "(" false_dest_operands ":" type(false_dest_operands) ")" }
           attr-dict }
```

`Custom` 只剩上游真的手写的那些 op（quant、emitc 的 C 表达式、部分 scf 等）。

## 9. 工具与验证

```bash
cd ana-docs/mlir
bison -Wall -Wcounterexamples -Werror -o /tmp/dg.c dialect-grammar.y   # 58 规则 / 142 产生式 / 0 冲突
python3 tools/dialect-corpus.py        # 四段审计 + gap 计数
python3 tools/dialect-coverage.py      # 覆盖缺口必须为 0
python3 tools/escape-audit.py          # L0–L6；失败退出 1
```

三份登记表是唯一的扩展点：

| 文件 | 列 | 约束 |
| --- | --- | --- |
| `tools/fact-glossary.tsv` | fact / scope / owner / meaning | 41 核心 + 20 本地；新词必须先登记 |
| `tools/gap-registry.tsv` | gap / witness / missing / status | 见证必须与语料一致 |
| `tools/hook-registry.tsv` | hook / dialects / reason | 只允许上游找不到同名 `custom<>` 的标签 |

## 10. 结论

1. **逃逸点的约束不在"能不能写"，在"写了算不算数"。** 19 章让通道显式，
   24 章让通道可证伪：`Custom`/`Generic`/`Gap` 都能被上游 TD 否定。
2. **假陈述比省略更坏。** 省略至少能被 R1 抓到；把声明式文本标成 `Custom`
   会让文法永远看不到那一段结构。L1 第一次跑就抓到 16 处。
3. **缺口要有名字。** `Gap(ref_type)` 比 `Custom` 多说三件事：缺什么、
   哪些 op 需要、怎么关。预算让它保持可关闭。
4. **本地标签可以存在，但要登记。** 钩子名是文本层的实现细节，不是上游事实；
   登记表让"随手造名字"和"有意的领域抽象"区分开。
5. **描述与断言分开之后，预算才有意义。** 22 个设计分歧不占逃逸预算，
   逃逸预算只盯断言；两条线各自有上限，互不掩盖。
