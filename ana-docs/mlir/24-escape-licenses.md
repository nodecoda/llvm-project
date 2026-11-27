# 24. 逃逸点契约：通道、范围、组合、退役

> 配套：`tools/escape-audit.py`（L0–L11，`--selftest` 29 例，失败退出 1）、
> `tools/gap-registry.tsv`（债务账本）、`tools/hook-registry.tsv`、
> `tools/fact-glossary.tsv`；文法：`dialect-grammar.y` 的 `Gap(name)`、
> `ref(...)`、`else`、`oilist<sep>`；上位规则见 [19 章](19-escape-hatches.md)。
> 语料：50 个方言、2777 个 op（本章数字随语料更新；历史数字见各轮章节）。

## 0. 问题：许可证写得出来，就等于没有许可证

19 章规定了"哪些通道、什么时候用、怎么计数"，24 章上一版把每个通道写成
四元组 `<准入, 范围, 义务, 检测>`。这还不够。只要通道本身没有形状约束，
它就会以三种方式重新变成黑洞：

| 滥用 | 上一版为什么拦不住 | 本轮的反例 |
| --- | --- | --- |
| **假陈述**：把声明式文本标成 `Custom` | 只检查"有没有写"，没检查"写的和上游是否一致" | 第一次跑 L1 抓到 16 个 op |
| **近似实参**：钩子只传一部分参数，假装覆盖了 | 只检查钩子名存在，没检查实参 | 实参形状检查共抓到 9 个 op（§5） |
| **永久缺口**：`Gap` 写一次就永远在，没人负责关 | 只检查名字登记，没有期限与关闭证据 | 3 条缺口全部在本轮关闭（§5） |
| **沉默的部件**：块只打印声明的一部分 | 只检查"引用能解析"，没检查"每个声明都出现" | L10 抓到 `emitc.include` 的 `is_standard_include`（§5.6） |
| **家族冒充部件**：`operands`/`results` 写错也没人管 | 这两个词在审计器里被当成指令跳过 | 679 处族引用此前从未核对（§5.6） |
| **守卫与声明矛盾**：`if x` 守着必选属性 | 只检查 `if` 的形状，没检查 `x` 能不能缺失 | L11 抓到 8 处声明不准确（§5.6） |

共同病根：**逃逸点的自由度没有被语法本身约束**。`Gap` 能写在 `if` 里，
`ref` 能无限嵌套，`else` 能塞进任意结构，钩子实参可以随手加减。这些都不是
"证据"问题，是**形状**问题——文法允许的东西，审计器再严也只能事后抱怨。

所以本章加一层：**逃逸点的语法本身是窄的**（§3 的 E4、§4 的 S5–S7），
审计器只负责证明"窄的形状用对了地方"。

## 1. 断言与描述

| 类别 | 记号 | 它说了什么 | 证据 |
| --- | --- | --- | --- |
| **断言** | `Custom` / `Generic` / `custom<Hook>` / `Gap(name)` | 关于**上游**或**本文法**的事实 | 必须能被 TD 或登记表核对 |
| **描述** | `Assembly { ... }` | 本文法自己定义的文本形态 | 只需内部自洽（引用可解析） |

`Custom` 说"上游有手写打印器"，`Generic` 说"上游没有自定义格式"，
`Gap(p)` 说"上游有声明式格式，但本文法缺 `p` 这个构造"。三者都是断言，
都能被证伪，因此都必须带证据。块格式不是断言，但它受另一套约束：范围（§4）。

## 2. 五通道许可证

| 通道 | 准入（evidence） | 范围（只覆盖） | 义务 | 检测 |
| --- | --- | --- | --- | --- |
| block | 无（描述） | 已声明部件、字面量、`type`/`attr-dict`/`oilist` 等指令 | 引用必须可解析 | L0 + 悬挂引用 |
| `Custom` | `hasCustomAssemblyFormat = 1`（含继承，`1` 或 `true`） | 该 op 的**全部**文本 | 结构仍在子句里声明 | L1 |
| `Generic` | 既无 `assemblyFormat` 也无手写打印器 | 无自定义文本 | 结构仍要声明 | L2 |
| `custom<Hook>(a,b)` | 钩子名在上游 TD 闭包里出现，或在 `hook-registry` 登记 | 只覆盖实参列出的部件 | 实参必须与上游逐位对应 | L3 + **L3b** |
| `Gap(name)` | 该名字在 `gap-registry` 登记为 `open`，见证集合精确 | **只表示缺失的那一处**；块其余部分照常描述 | 一个 op 至多一个，写在文本脊上 | L4 + L7 |

### 2.0 家族引用不是第六种通道

块里允许写 `operands` / `results`，它们是**家族引用**——一次打印整个家族，
不是逃逸。它们看起来像指令，所以更容易变成黑洞；本版把它们的三条边界写死：

| 名字 | 只覆盖 | 不覆盖 | 何时是家族 |
| --- | --- | --- | --- |
| `operands` | 全部操作数的**值** | 类型（要 `type(operands)`）、属性、region | 没有同名部件时 |
| `results` | 全部结果的**类型列表**（只在 `type(...)` / `functional-type(...)` 实参里） | 结果值、属性 | 没有同名部件时 |

三条硬边界：

1. **软名字**：若本 op/schema 链声明了名为 `operands`/`results` 的部件，
   块里的同名 token 是**部件**，家族速记被遮蔽（S8）。`transform.apply_patterns`
   的 `operands`、`memref.alloca_scope.return` 的 `results` 都属于这种。
2. **只覆盖一维**：`operands` 不打印类型，`type(operands)` 不打印值；
   想两者都要就各写一次——这正是上游 tosa 的形状。
3. **不是数据**：钩子实参只接受部件，`custom<F>(operands)` 在没有同名部件时
   直接失败（L10）。家族是文本层面的缩写，不是可传给 C++ 的值。

空家族是合法的：上游 `tosa.const` 没有操作数却写 `operands`，打印出空串；
`functional-type(operands, results)` 在结果为空时打印 `()`。所以"家族为空"
本身不是错误，**冒充部件**才是。

### 2.1 证据的权威来源

上游集合不再由正则扫描 `.td` 文本推断，而是由 `llvm-tblgen --dump-json`
给出每个 op 的 `opName`、`assemblyFormat`、`hasCustomAssemblyFormat`。
理由是拼接助记符（`"convert." # !tolower(src) # "x2.to.f4x2"`）、
`multiclass` 展开与 `foreach` 生成的名字对正则不可见；看不见就会"静默跳过"，
而静默跳过正是本章要消灭的黑洞。缺 `LLVM_TBLGEN` 时审计器直接失败
（`L0 ... set LLVM_TBLGEN`），不降级为"看起来全绿"。

另有一条**规范化**（不是逃逸）：上游 `enum($x)` 表示"用属性 x 的枚举打印器
输出助记符"，本文法没有单独的 `enum()` 指令，块里直接引用部件 `x`。
这个改写是全域、机械的，并且被 **N1** 逐 op 检查：上游打印 `enum($x)`，
块就必须出现 `x`。本轮 N1 抓到 gpu/sparse_tensor 的 17 个 op 漏写枚举部件。

两条容易混的边界：

- `custom<Hook>` 的**名字**是本文本的标签，不要求与上游同名；但若上游有同名
  `custom<>` 就优先用同名，找不到对应物时必须在 `hook-registry` 解释。
  这条检查抓到过真错误：`emitc.global` 把上游
  `custom<EmitCGlobalOpTypeAndInitialValue>` 写成了
  `custom<GlobalTypeAndInitialValue>`——语法合法、名字悬空。
- `Gap(name)` 不是第六种逃逸。它不提供"随便写"的自由：块还是要写，
  只是允许在**一个具名位置**承认缺口。缺口有名字、有见证、有期限（§5）。

## 3. 设计规则（E0–E10）

这一节是本章新增的**契约正文**。每条规则都说明"为什么"，并且都能被指到
一个检测器；没有检测器的规则不写进来。

| # | 规则 | 为什么 | 检测 |
| --- | --- | --- | --- |
| E0 | **通道是封闭集**：block / Custom / Generic / hook / Gap，五种。新增一种要同时改文法、登记表、审计器和本章 | 通道越多，"该用哪个"越靠自由心证 | 文法只有 `assembly` 的三种形态与块内两种逃逸 |
| E1 | **每个 op 恰好一个文本通道**；schema 继承只沿一条链上溯，取最近声明 | 两个通道同时生效时，"谁负责这段文本"没有答案 | L0 + `effective_block`（`escape-audit.py:262`） |
| E2 | **断言必须可证伪**：Custom/Generic/hook/Gap 各自对应一条上游或登记证据 | 不可证伪的断言就是注释 | L1 / L2 / L3 / L4 |
| E3 | **实参即范围**：钩子实参必须与上游调用的数据参数**逐位相等**；字符串配置两侧同时丢弃 | 少传一个参数，钩子看到的世界就比上游小；多传一个，就引入了上游没有的耦合 | **L3b** |
| E4 | **逃逸不组合**：一个 op 至多一个 `Gap`；`Gap` 只在文本脊上；`ref` 只包一层；`else` 只打印字符串 | 组合的逃逸无法用一张许可证描述，也无法逐条审计 | L7 + 文法（`ref_arg:341-352`、`else_opt:357-365`） |
| E5 | **缺口有期限**：登记行必须有 `scope`、`retire_when`；关闭要有 `closed_by`；`open` 与语料见证精确一致，`closed` 不得回潮 | 没有期限的缺口 = 永久豁免 | L4 + `gap_findings()`（`escape-audit.py:385`） |
| E6 | **预算分层**：全局 Custom ≤ 25%、Gap ≤ 12、分歧 ≤ 30；每方言 Custom ≤ 80%、Gap ≤ 3 | 全局比例会掩盖单方言的失控（affine 69%、scf 58%） | L6 + L9 |
| E7 | **逃逸不携带语义**：事实只从 `Properties`/`Implements` 来；本地事实不得跨方言 | 否则逃逸点会变成"随便加谓词"的后门 | L5 |
| E8 | **家族名是软名字**：`operands`/`results` 只在没有同名部件时表示家族；只覆盖该家族；不得作为钩子实参 | 否则"打印全部"会变成"打印我想打印的" | L10 + S8 |
| E9 | **覆盖收口**：块的每个已声明 operand/region/successor 必须出现在文本里，attribute/property 必须被命名或被 dict 收口；`Gap` 不豁免覆盖 | 只打印一半声明也能通过，等于块在撒谎 | L10 |
| E10 | **守卫必须可缺失**：`if x` 要求 `x` 在声明里 optional/variadic/有默认值/是 unit；region 例外（缺失即空 region） | 文本承诺"可能不打印"，声明承诺"一定存在"，必有一边错 | L11 |

E3 是这一轮最重要的新规则：它把"钩子名对"升级成"钩子看到的部件对"。
证据是固化前的手工比对加 L3b 第一次运行，一共抓到 9 个 op 的实参偏差（§5.1）。

E4 的三条窄化直接落在文法里，而不是审计器里：

| 逃逸构造 | 上一版的自由度 | 现在的形状 | 文法 |
| --- | --- | --- | --- |
| `ref(...)` | 不存在（只能绕过） | `ref(x)` 或 `ref(type(x))`，不嵌套 | `ref_arg`，`dialect-grammar.y:348` |
| `else` | 不存在（只能 `Gap`） | `else { "literal" ... }`，只打字符串 | `asm_string_list`，`:362` |
| `oilist` 分隔符 | 不存在（只能 `Gap`） | `oilist<"sep">(...)` 一个字符串 | `oilist_sep`，`:371` |
| `Gap` 位置 | 任意 `asm_element` | 仍在块内，但 L7 禁止出现在 `if`/`oilist`/钩子实参里 | L7 |

## 4. 范围细则（scope）

通道的自由度由"它能提到什么"决定。七条硬边界：

| # | 规则 | 违反的样子 | 检查 |
| --- | --- | --- | --- |
| S1 | 块里的标识符必须是本 op/schema 链上已声明的部件或指令 | 写了一个没声明的名字 | 悬挂引用 |
| S2 | `custom<Hook>(...)` 的实参只能是已声明部件，或 `type(x)`/`ref(...)` 派生 | 钩子顺手引入新部件 | 悬挂引用 + L3 |
| S3 | `Gap(name)` 只能出现一次且位于文本脊；块其余部分必须能独立解析 | 用 Gap 把整段文本吞掉 | L4 + L7 |
| S4 | 事实只能描述已声明部件；本地事实只属于一个方言 | 借别的方言的词 | L5 |
| S5 | **钩子实参是数据，不是配置**：字符串字面量（分隔符、builder 名）不进实参 | 把上游的实现开关抄进领域模型 | L3b 两侧同时丢弃字符串 |
| S6 | **`else` 只打印固定文本**：`else { "undef" }`，不能放部件或结构 | 把缺省分支写成第二个格式 | 文法 `asm_string_list` |
| S7 | **逃逸的证据是逐 op 的**：同名钩子在不同 op 里仍要各自通过 L3b；方言或族不能批量继承许可证 | "这个方言都这样" | L3b 按 op 比较 |
| S8 | **家族名被同名部件遮蔽**：声明了 `operands`/`results` 部件时，块里的同名 token 是部件，不是家族 | 把"一个部件"当成"全部操作数" | L10 + `parts` 表 |
| S9 | **`Gap` 不豁免覆盖**：缺口只允许一个具名构造缺失，块其余部分必须独立完整 | 用 Gap 把整段声明藏起来（`func.call_indirect` 的旧写法） | L10 + L7 |
| S10 | **region 覆盖按通道分**：声明式上游必须打印 region；custom 上游的 region 体由框架打印，不在块里 | 把 body 塞进 `ins(...)` 里，或反过来假装 region 不存在 | L10（`upstream == block` 才查 region） |

S3 第一次落地时抓到一个真问题：`func.call_indirect` 的 `Gap(oilist_separator)`
把 `arg_attrs`/`res_attrs` 整个吞掉了，块里只写 `callee "(" callee_operands ")"`，
而这两个属性根本没在 `Attributes` 里声明。缺口一关，悬挂引用立刻暴露——
**缺口会掩盖声明缺失**，这比缺口本身更危险。

## 5. 本轮关闭的三条缺口

缺口不是被预算挤掉的，是被语料和文法补上的。三条登记行从 `open` 变成
`closed`，`closed_by` 记录关闭它的产生式与见证。

### 5.1 `ref_type` → `A_REF`

上游有 9 个 op 的钩子实参里出现 `ref(...)`（语法形状加实参形状两轮检查的并集）：

| op | 旧实参 | 上游实参形状 | 处置 |
| --- | --- | --- | --- |
| `cf.switch` | `flag` | `ref(type(flag))` | 改写首参 |
| `gpu.launch_func` | `type(grid_size_x)`（注释自认"近似"） | `T, ref(cluster_size_x), T, T, T` | 按上游写全 |
| `pdl.range` | `type(arguments)` | `ref(type(arguments))` | 改写 |
| `pdl.results` | `index` | `ref(index)` | 改写 |
| `pdl_interp.create_range` | `arguments` | `ref(type(arguments))` | 改写 |
| `omp.iterator` | 4 个 | 7 个（缺 3 个 `type(...)`） | 补全 |
| `omp.interop.init/use/destroy` | 4 个 | 6 个（缺 2 个 `type(...)`） | 补全 |

这 9 个 op 是形状检查的并集结果：cf/gpu/pdl/pdl_interp 由 `ref` 产生式暴露，omp 两族由 L3b 暴露。`gpu.launch_func` 的注释写着
"用单参形式近似该钩子"——这句话本身就是滥用：**近似不是许可证**。
现在文法里有 `ref_arg`，近似没有存在的理由。

### 5.2 `oilist_separator` → `oilist_sep`

`func.call_indirect` 的上游是 `oilist<\`,\`>(...)`。上一版块里写
`callee "(" callee_operands ")" attr-dict`，用 `Gap` 标记分隔符缺失，
但也顺手把 `arg_attrs`/`res_attrs` 两支藏掉了。现在写成：

```text
Assembly { callee "(" callee_operands ")"
           oilist<",">("arg_attrs" "=" arg_attrs | "res_attrs" "=" res_attrs)
           attr-dict ":" type(callee) }
```

### 5.3 `optional_default` → `else`

上游 `($x^):(\`undef\`)?` 是"可选组的缺省分支打印固定字面量"。三条见证
（`amdgpu.packed_trunc_2xfp8`、`packed_scaled_trunc`、`packed_stoch_round_fp8`）
逼出 `else`：

```text
Assembly { attr-dict sourceA "," if sourceB { sourceB } else { "undef" }
           "into" if existing { existing } else { "undef" }
           "[" "word" wordIndex "]" ":" type(sourceA) "to" type(res)
           if existing { "into" type(existing) } }
```

`else` 体只接受字符串字面量。缺省分支是"打印固定文本"，不是第二个格式；
想放结构就走正常分支，或者给文法加一个具名产生式。这条限制是 S6。

### 5.4 `qualified` 作用于属性

上游 `llvm.intr.dbg.declare` / `dbg.value` 打印 `qualified($varInfo)`、
`nvvm.barrier.reduction` 打印 `qualified($reductionOp)`——这些是**属性**，不是类型。
旧文法只有 `qualified(type(x))`，语料只好"按裸名打印"并在 Doc 里道歉；这正是
S2 说的"派生形式没覆盖到，就退化成近似"。现在加 `qualified(x)`（`x` 必须是
已声明部件），三个 op 恢复原样。`qualified` 只能包一层，不允许 `qualified(qualified(x))`。

### 5.5 债务账本

`gap-registry.tsv` 现在是**账本**，不是待办清单：

| gap | scope | status | 关闭依据 |
| --- | --- | --- | --- |
| `optional_default` | optional-group | closed | `else_opt`；3 个 amdgpu 见证 |
| `ref_type` | hook-args | closed | `A_REF`；cf/pdl/pdl_interp/gpu/omp 6 个见证 |
| `oilist_separator` | oilist | closed | `oilist_sep`；func.call_indirect |

`closed` 行不允许再出现在语料里（L4）。账本保留历史行，因为"曾经缺什么、
怎么关的"是下一轮判断的依据。

`A_GAP` 因此成为文法里唯一没有语料见证的逃逸产生式。它保留，不是特权，
而是**待用通道**：L4 要求任何使用都带登记与精确见证，L7 要求它只在文本脊，
`--selftest` 覆盖了 unregistered / witness-drift / closed-but-used /
stale / missing-scope / scope-mismatch 六种拒绝路径。若下一轮语料仍无见证，
按 E0 的对称性，它应当从文法删除，五通道降为四通道。

### 5.6 本轮新增的三条约束（以及它们抓到的洞）

覆盖与守卫不是"更严的预算"，它们检查的是**块与声明是否说同一件事**。
这一轮把三个此前完全没被检查的形状变成 L10/L11：

| 洞 | 之前为什么看不见 | 现在的检查 | 抓到 |
| --- | --- | --- | --- |
| 家族名从未解析 | `operands`/`results` 被当成指令跳过，既不算引用，也不算错 | 软名字 + 覆盖 + 钩子实参 | 679 处族引用首次被核对 |
| 块可以少打印 | 只查"引用能解析"，不查"声明都出现" | L10 覆盖 | `emitc.include` 的 `is_standard_include` 无处落地 |
| 守卫与声明矛盾 | 只查 `if` 的形状 | L11 可缺失性 | 8 处声明不准确（下表） |

L11 的 8 处全部是**声明层把"可缺失"写成了"必选"**。上游证据：

| op | 字段 | 上游 | 语料修正 |
| --- | --- | --- | --- |
| `nvvm.mbarrier.init` | `predicate` | `PtxPredicate = Optional<I1>` | `optional<i32>` |
| `nvvm.mbarrier.arrive.expect_tx` | `predicate` | 同上 | `optional<i32>` |
| `nvvm.prefetch` | `predicate` | 同上 | `optional<i32>` |
| `nvvm.inline_ptx` | `predicate` | 同上 | `optional<i32>` |
| `nvvm.movmatrix` | `layout` | `DefaultValuedAttr<MMALayoutAttr,"col">` | `= col` |
| `rocdl.asyncmark` | `stages` | `DefaultValuedAttr<ROCDL_AsyncStagesAttr,"all">` | `= all` |
| `rocdl.wait.asyncmark` | `stages` | 同上 | `= all` |
| `transform.apply_registered_pass` | `options` | `DefaultValuedAttr<DictionaryAttr,"{}">` | `= {}`（文法新增空字典默认值） |

L10 的两处修正：

- `emitc.include` 原来写 `oilist("<" include ">" | include)`。`oilist` 允许两支
  同时出现，而这两种形态互斥；`is_standard_include` 也不在文本里。现在写成
  `if is_standard_include { "<" } include if is_standard_include { ">" }`：
  同一份文本，两个受守卫的字面量，属性被看见。
- `llvm.intr.coro.free` 少了 `Results (res: llvm_ptr)`，块里的
  `functional-type(operands, results)` 却在引用结果家族。补上声明后，
  "结果为空"与"声明了结果"不再自相矛盾。

还有一个**审计器自己**的黑洞：`assembly_block` 用 `seg.find("Assembly")` 找
子句，于是注释里的 "Assembly Generic。" / "AssemblyFormat=1" 被当成格式，
6 个 op 的块被读成注释文本（`arm_sve.masked.*`、`pdl_interp.check_result_count`、
`linalg.scaled_contract` 等）。现在锚定**行首**的 `Assembly` 子句，并只取
配对的 `{...}`。这条修复没有改变任何许可证，却让 L10 的输入从"注释"变回"块"。

## 6. 检测器（L0–L11）

`tools/escape-audit.py` 对每个方言做四件事：从语料取通道与形状，从上游 TD
闭包取证据，从登记表取许可证，最后按预算结算。

| 规则 | 证明什么 | 失败意味着 |
| --- | --- | --- |
| L0 | 每个 op 都交代了文本来源 | 有 op 在静默继承"没有格式" |
| L1 | `Custom` 的上游真有手写打印器 | 假陈述 |
| L2 | `Generic` 的上游真没有自定义格式 | 用 Generic 逃避描述 |
| L3 | 钩子名要么上游有，要么登记过 | 悬空钩子名/拼写错误 |
| **L3b** | 钩子实参与上游数据参数逐位相等（字符串配置丢弃） | 钩子看到的世界比上游小/多 |
| L4 | 缺口登记、见证精确、scope 与文本匹配、closed 不回潮 | 缺口被滥用或账本腐化 |
| L5 | 事实登记、本地事实不跨方言 | 事实变成自由注释 |
| L6 | 全局预算 | 逃逸总量失控 |
| L7 | 一个 op 至多一个 Gap，且不在组/钩子实参里 | 逃逸组合 |
| L9 | 每方言预算（Custom ≤ 80%、Gap ≤ 3） | 单方言失控被全局比例掩盖 |
| **L10** | 块覆盖：operand/region/successor 出现，attribute/property 被命名或收口；家族名只在无同名部件时成立，且不是钩子数据 | 块在"选择性打印"，或家族名冒充部件 |
| **L11** | `if x` 的 `x` 可缺失（optional/variadic/default/unit；region 例外） | 文本与声明对"能不能缺"说反了 |

L3b 的实现要点：上游实参先归一化成 `V` / `T` / `R(V)` / `R(T)`，字符串
字面量返回 `None` 并被丢弃；语料侧用同一函数归一化，然后逐 op 比较。
`ref(ref(x))` 在文法层就不存在，所以归一化不需要处理嵌套。

`--selftest` 把每一类拒绝路径钉成用例（29 例）：

```bash
python3 tools/escape-audit.py --selftest   # 29 cases, 0 failed
```

零 open 缺口时，L4 的正常路径是空集；L10/L11 也可能因为语料恰好干净而全绿。
selftest 用合成部件把每条规则的拒绝路径钉住，保证绿灯不是"没检查"。

## 7. 预算与账本（当前）

| 指标 | 当前 | 红线 | 说明 |
| --- | --- | --- | --- |
| 方言 / op | 50 / 2777 | — | 语料规模 |
| 块通道 op（L10 的覆盖对象） | 2523 | — | 其余 254 个是 Custom/Generic |
| 家族引用 | 679 处 / 381 op | — | `operands`/`results`，软名字规则 S8 |
| `if` 守卫 | 912 | — | 全部通过 L11 可缺失性 |
| 无文本通道的 op | 0 | 0 | 每个 op 都交代了文本来源 |
| `Custom` 占 op 比 | 122/2777 = 4.4% | 25% | 上游手写打印器 |
| `Generic` | 132 | — | 上游无格式的 op |
| 具名缺口 | 0 | 12 | 三条全部关闭（§5） |
| 钩子调用 | 396 | — | 每个都过 L3 + L3b |
| `enum($x)` 规范化 | 逐 op | — | N1：上游打印枚举助记符，块里必须出现该部件 |
| 模板格式无法判定 | 0 | — | tblgen 逐 op 解析，不再有 `!if` 悬念 |
| 单次使用的本地事实 | 35% | 50% | 41 核心 + 20 本地 |
| 设计分歧（block vs 上游 custom） | 7 组 / 71 op | 30 组 | 按 schema 去重，见 §8 |

每方言账本由审计器直接输出（`## per-dialect escapes`），两个离群值：

| 方言 | ops | Custom | 份额 | 原因 |
| --- | --- | --- | --- | --- |
| `affine` | 16 | 11 | 69% | 上游确实为 affine 的循环/访存逐 op 写了打印器 |
| `scf` | 12 | 7 | 58% | `for`/`if`/`while`/`parallel` 等都是手写格式 |
| `spirv` | 430 | 33 | 8% | 397 个 op 有声明式格式，33 个只有手写 parser/printer |
| `llvm` | 323 | 8 | 2% | 其余走 block/Generic，intrinsic 族基本没有格式 |
| `linalg` | 78 | 7 | 9% | 结构化算符上游全部手写打印；语料用一个 `Structured` schema 重建 |

两者都通过 L1（上游真有 `hasCustomAssemblyFormat`），所以是"上游的选择"，
不是本文法的逃逸。E6 的 80% 上限特意留出这个空间：**预算是为了发现异常，
不是为了把上游的真实形态改写成好看的数字**。

## 8. 设计分歧不是逃逸

有 71 个 op 的块格式与上游"不一致"，但只来自 **7 个设计决定**：

| 组（按 schema 去重） | op 数 | 上游形态 | 性质 |
| --- | --- | --- | --- |
| `linalg.Structured` 收敛的命名算符 | 65 | 手写打印器 | 一个 schema 重建上游逐 op 手写的 `ins(...) outs(...)` |
| `linalg.yield` | 1 | 手写打印器 | 文本就是操作数列表 |
| `affine.vector_load` / `vector_store` | 2 | 手写打印器 | 访存修饰由手写打印器给出，语料用 `oilist` 重建 |
| `arith.select` | 1 | 手写打印器 | `cond ? a : b` 与属性形式 |
| `emitc.include` | 1 | 手写打印器 | include 路径与常量两种形态 |
| `smt.int.constant` | 1 | 手写打印器 | 定值打印 |

这些都不是逃逸：没有跳过任何结构，块里的每个引用都能解析。它们是
**描述**层面的设计选择，所以 L1/L2 不管，只计数并设上限。

**计数单位是决定，不是实例**：同一个 schema 收敛的一族 op 是一个设计决定。
按 op 计数会让 `linalg` 的 65 个同形算符把预算撑爆，从而掩盖真正的异常；
按组计数后，任何新组都会立刻可见。超过上限时先问：
"是不是块格式缺了某个构造，才逼得语料去猜上游？"

## 9. 工具与验证

```bash
cd ana-docs/mlir
bison -Wall -Wcounterexamples -Werror -o /tmp/dg.c dialect-grammar.y   # 62 条规则 / 153 条产生式 / 0 冲突
python3 tools/dialect-corpus.py        # 四段审计 + gap 计数
export LLVM_TBLGEN=/path/to/build/bin/llvm-tblgen
export LLVM_BUILD_DIR=/path/to/build
python3 tools/dialect-coverage.py      # 50 个方言必须 missing=extra=0
python3 tools/escape-audit.py          # L0–L11 + N1；失败退出 1
python3 tools/escape-audit.py --selftest   # 29 例拒绝路径
```

三份登记表是唯一的扩展点：

| 文件 | 列 | 约束 |
| --- | --- | --- |
| `tools/fact-glossary.tsv` | fact / scope / owner / meaning | 41 核心 + 20 本地；新词必须先登记 |
| `tools/gap-registry.tsv` | gap / scope / witness / missing / retire_when / closed_by / status | open 见证精确；closed 有依据且不回潮 |
| `tools/hook-registry.tsv` | hook / dialects / scope / reason / retire_when | 只允许上游找不到同名 `custom<>` 的标签 |

## 10. 结论

1. **逃逸点的约束要落在两层**：文法层管形状（E4/S5–S7），审计层管证据
   （E2/E3/E5）。只有审计层，逃逸点仍可以长成任意形状；只有文法层，
   合法的形状也会被用错地方。
2. **假陈述比省略更坏。** 省略至少能被 L0 抓到；把声明式文本标成 `Custom`
   会让文法永远看不到那一段结构。L1 第一次跑就抓到 16 处。
3. **近似不是许可证。** `gpu.launch_func` 的"单参近似"、`omp.iterator` 的
   "少传三个 type"，在 L3b 下都是失败。钩子的实参就是它的世界边界。
4. **缺口要还。** 三条缺口在同一轮里被三条产生式关掉，账本从待办变成历史。
   留下来的 `Gap` 产生式没有见证，它的合法性只来自"随时可被 L4 拒绝"。
5. **预算要分层。** 全局 5.3% 的 Custom 掩盖不了 affine 的 69%；分方言账本
   把异常点名，同时允许上游的真实形态存在。
6. **"能解析"不等于"说完整"。** 块通道过去只保证引用不悬空；L10 把
   "每个声明部件都被看见"变成硬条件。覆盖检查抓到的不多（1 处），但它
   堵住的是整类沉默：只打印一半声明，文法与审计都不会抱怨。
7. **家族名是最容易被当成指令的黑洞。** `operands`/`results` 在 679 处出现，
   此前没有任何检查。软名字规则（S8）让"是部件还是家族"由声明决定，
   而不是由拼写猜测；这正是把语义层的名字解析写清楚、而不是塞进词法的例子。
