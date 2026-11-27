# 22. 第七轮语料：amdgpu / openmp / smt

> 配套文件：[`corpus/amdgpu.dialect`](corpus/amdgpu.dialect)（858 行、47 op）、
> [`corpus/openmp.dialect`](corpus/openmp.dialect)（1374 行、72 op）、
> [`corpus/smt.dialect`](corpus/smt.dialect)（536 行、54 op）；
> [`dialect-grammar.y`](dialect-grammar.y)（**58 条规则 / 138 条产生式**，0 冲突）；
> 28 个语料文件全部 `parse OK`。
>
> 与 21 章的关系：第三次压测，也是规模最大的一轮（173 个 op）。0 条新产生式；
> `speculatable` 因第二个方言见证升级进核心（40 → 41）。

## 0. 这一轮选这三个的理由

| 方言 | 检验点 |
| --- | --- |
| `amdgpu` | 类型域爆炸：41 个约束覆盖 fp8/fp6、MFMA/WMMA、描述符与 barrier 缓冲；47 个 op 里 4 个是模板派生 |
| `openmp` | 子句是文本的极端形态：70 个 `custom<>` 钩子、24 个枚举、29 处属性定长的变长操作数 |
| `smt` | 纯类型层 + 高规整家族：6 个 sort、54 个 op（位向量 21 + 整数 9 + 数组 3），13 个二元位向量 op 共用一个 schema |

三者分别压 **约束数量**、**子句文本**、**类型层规整性**。

## 1. 语料

| 文件 | 行数 | dialect | constraint | enum | attr_def | type_def | schema | op |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `amdgpu.dialect` | 858 | 1 | 41 | 3 | 3 | 4 | 11 | 47 |
| `openmp.dialect` | 1374 | 1 | 12 | 24 | 1 | 4 | 10 | 72 |
| `smt.dialect` | 536 | 1 | 3 | 2 | 1 | 6 | 8 | 54 |
| 其余 25 个方言 | 5741 | 25 | 66 | 34 | 14 | 33 | 76 | 519 |
| **合计** | **8509** | **28** | **122** | **63** | **19** | **47** | **105** | **692** |

## 2. 文法增量：0 条产生式，核心词表 +1

173 个 op、三个领域，没有一条新产生式。词表动了一次：

### 2.1 `speculatable` 升级进核心（40 → 41）

`sparse_tensor` 早就在用 `speculatable`（2 处），`smt` 的 `forall`/`exists`
独立需要同一个概念（`RecursivelySpeculatable`：量化体可被推测执行）。
按 19 章 R5 第 4 条，两个方言的独立见证足以进核心：

| 动作 | 结果 |
| --- | --- |
| `speculatable` 移入核心表 | 核心 40 → **41** |
| 本地词表 | 18 → **17** |
| 单次使用 | 6（35%） |

连续两轮都出现"新方言不带新词、只把已有本地词升级"的模式，说明词表的
增长点已经从"补缺口"转向"归并同义词"。

## 3. amdgpu：类型域比 op 形状更重

47 个 op、11 个 schema、**41 个约束**——约束是 op 数的近一倍。原因：

- 矩阵指令的操作数类型是"元素类型 × 尺寸"的笛卡尔积（fp8/fp6/bf16/f32、
  MFMA/WMMA/SWMMAC），每个组合在 TD 里是一个约束；语料按领域类别命名，
  不为每个组合造词。
- 4 个 op 是模板派生（`make_dma_base`/`make_gather_dma_base`/
  `make_dma_descriptor`/`make_gather_dma_descriptor`），按具体 op 记录，
  不按模板记录——**声明名是打印名，不是 C++ 类名**（15 章的规则在模板上同样成立）。
- 3 个 op 走 `Assembly Custom;`（`packed_trunc_2xfp8` 等），因为上游在缺省
  可选操作数位置打印字面量 `undef`；块格式没有"缺省文本"这一元素（§8）。
- 3 个 attr_def 是枚举属性（`dpp_perm`/`load_temporal_hint`/`cache_scope`），
  与枚举同形；`address_space` 没有声明，因为它只以 memref 内存空间出现，
  不是可声明的类型。

## 4. openmp：子句把文本层推到极限

72 个 op、**24 个枚举**、**70 个 `custom<>` 钩子**、29 处
`operand_segments_by_attribute`。这是目前对文本层压力最大的方言：

- 子句列表（`private`/`reduction`/`map`/`depend`/`schedule`/`affinity` …）
  在 TD 里就是一个个 `oilist(...)` + 子钩子；块格式能表达结构，
  子句内部的列表交给 `custom<...>`。**这不是逃逸，是分工**：结构在事实层，
  列表文本在钩子层。
- 5 个 `Assembly Custom;` 与 TD 的 `hasCustomAssemblyFormat` 一一对应
  （`canonical_loop`/`unroll_heuristic`/`unroll_full`/`unroll_partial`/`loop_nest`），
  没有多一个也没有少一个——R2（通道只覆盖文本）的自动核对在这里最有效。
- region 事实全部落在 region 上：`single_block` 只写 TD 明说
  `SingleBlock`/`SizedRegion<1>` 的地方，其余用 `implicit_terminator(terminator)`
  记录"结构性出口"，不替上游补 `single_block`。
- **0 个本地事实**：135 次事实使用全部来自核心 41 词。子句最复杂的方言
  没有逼出新词，是对词表最好的压力测试。

## 5. smt：类型层与家族压缩

54 个 op（`SMTOps.td` 21 + `SMTBitVectorOps.td` 21 + `SMTIntOps.td` 9 + `SMTArrayOps.td` 3），6 个 `type_def`：

```text
!smt.bool   !smt.int   !smt.bv<width>   !smt.array<[domain -> range]>
!smt.func<(domain) range>              !smt.sort<identifier[sort_params]>
```

- 51 个块格式、3 个 `Custom`（`eq`/`distinct` 的定制打印器只印一次公共类型；
  `bv.repeat` 的次数由结果/输入位宽导出，不是部件），0 个 Generic。
- 13 个二元位向量 op 共用一个 `bv_binary` schema；`and`/`or`/`xor` 与
  量词各共用一层；整数理论再用 `int_variadic`/`int_binary` 两层——家族压缩 54 → 8 schema。
- GAP：`AnyNonSMTType` 是**否定谓词**，类型并集表达不了"非 SMT 类型"，
  只能写成 `any` 并在 Doc 说明。

## 6. 共享名字空间的双面性

28 个文件共享一个名字空间（约束、枚举、类型可跨方言复用，17 章起就是设计）。
这一轮出现了它的副作用：`tosa` 一直引用的 `scalar` 原本是外部署名，
`amdgpu` 声明 `constraint scalar = signless_integer | float` 之后，
工具的外部名报告里 `tosa` 那一行消失了。

- **好的一面**：同一领域词只需定义一次，复用是显式的。
- **坏的一面**：R7 的"外部署名逐条列出"是**全局**判据，一个方言的拼写错误
  可能被另一个方言的同名声明掩盖。
- 判决：保留共享名字空间（跨方言复用是真实需求），但 R7 检查升级为
  "**本方言可见**"解析：先本文件，再核心/内建，最后才是其它方言的声明；
  其它方言的声明要在报告里单独列出，而不是静默通过。第八轮改工具。

## 7. 预算与趋势

| 轮次 | 方言 | 累计 op | 累计产生式 | 本轮新增 op | 本轮新增产生式 |
| --- | --- | --- | --- | --- | --- |
| 16 | 9 | 119 | 128 | 42 | 30 |
| 17 | 14 | 199 | 130 | 80 | 2 |
| 18 | 19 | 346 | 136 | 147 | 6 |
| 20 | 21 | 438 | 137 | 92 | 1 |
| 21 | 25 | 519 | 138 | 81 | 1（文本通道） |
| **22** | **28** | **692** | **138** | **173** | **0** |

| 指标 | 当前值 | 红线 |
| --- | --- | --- |
| `ops without a text channel` | 0 | 0 |
| `Assembly Custom` 占 op 比 | 75/692 = 11% | 25% |
| 单次使用的本地事实 | 6/17 = 35% | 50% |
| 跨 ≥3 方言的非核心事实 | 0 | 0 |
| 核心词表 | 41 | — |
| 外部署名 | 12 个 / 7 个方言 | 逐条可辨认 |

最后 346 个 op（第五至七轮）只换来 2 条产生式（都是文本通道）、2 个核心词、
本地词表净减 2（19 → 17）。

## 8. 缺口判决

| 缺口 | 见证 | 判决 |
| --- | --- | --- |
| 可选部件的缺省文本（`undef`） | amdgpu 3 处 | 记录：块格式缺"缺省字面量"，暂用 Custom |
| 否定谓词 `AnyNonSMTType` | smt | 记录：类型并集表达不了补集 |
| `qualified($attr)`（属性不是类型） | smt 的 `bv.constant` | 记录：`qualified` 只接受 `type(...)` |
| 内存空间作为类型参数 | amdgpu / ptr | 记录：接口约束缺位（21 章已列） |
| 24 个子句枚举 | openmp | **不是缺口**：枚举按打印拼写声明即可 |
| 模板派生的 4 个 op | amdgpu | **不是缺口**：按打印名逐个声明 |

## 9. 机械核对

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
| 语料解析 | 28/28 `parse OK` |
| op 名单 vs TD | 0 缺失、0 臆造（47/72/42 逐名单比对；amdgpu 含 4 个模板派生） |
| 部件写错子句（复核后） | 0（初稿 7 处，已修） |
| `from`/`extends` 悬空 | 0 |
| Assembly 引用未声明部件 | 0 |
| 死声明 | 0 |
| C++ 泄漏 | 0 |
| 无文本通道的 op | 0 |

事实核对抓到 **7 处部件写错子句 + 1 处通道错**，全部修正后才得到上表：

| 文件 | 初稿 | TD 事实 | 修正 |
| --- | --- | --- | --- |
| amdgpu `BufferAccess` | `sgprOffset` 记为属性 | `Optional<I32>:$sgprOffset` 是操作数（7 个 buffer op） | 移入各 op 的 Operands |
| openmp `TargetDataStandalone` | `if_expr` 记为属性 | `Optional<I1>:$if_expr` 是操作数（3 op） | 移入 Operands |
| openmp `dispatch` | `nowait` 记为操作数 | `UnitAttr:$nowait` 是属性 | 移入 Attributes |
| openmp `wsloop` | `schedule_chunk` 记为属性 | `Optional<AnyType>:$schedule_chunk` 是操作数 | 移入 Operands |
| openmp `task` | `priority` 记为属性 | `Optional<AnyInteger>:$priority` 是操作数 | 移入 Operands |
| openmp `taskloop.context` | 同上 | 同上 | 移入 Operands |
| openmp `cancel` | `if_expr` 记为属性 | `Optional<I1>:$if_expr` 是操作数 | 移入 Operands |
| smt `attr_def bv` | 块格式 | `hasCustomAssemblyFormat=true`、参数是 `APInt` | 改为 `Assembly Custom;` |

教训：**子句（clause）是最容易把 operand 写成 attribute 的地方**——TD 里
`Optional<I1>:$if_expr` 与 `UnitAttr:$nowait` 在同一个 `clauses = [...]` 列表里，
只差一个 `Attr` 后缀。工具只能查"引用是否存在"，查不出"在哪个子句"；
这一层必须靠逐条对照 TD。七处里六处在 openmp，说明子句越多的方言越需要
独立的核对清单。

## 10. 下一轮

1. 工具：外部名检查改成本方言优先（§6）。
2. 候选方言：`spirv`（434 个 def，枚举/属性最重）、`llvmir`（521 个 op）、
   `nvvm`（180）、`xegpu`。
3. 复审 17 个本地事实里剩下的 6 个单次项。

## 11. 结论

1. **规模不再带来文法增长。** 173 个 op → 0 条产生式；346 个 op → 2 条文本产生式。
2. **最复杂的方言用了最少的本地词。** openmp 70 个子句钩子、135 次事实、
   0 个本地事实——核心 41 词覆盖了子句语义。
3. **词表进入归并期。** 连续两轮的新词都是"别的方言已有的词"，升级进核心
   而不是新增本地词。
4. **共享名字空间要配"本方言可见"的检查。** 复用是特性，静默解析是缺陷；
   R7 的判据要改。
5. **Custom 的正当理由只剩一种**：块格式表达不了的缺省文本/派生文本
   （amdgpu 的 `undef`、smt 的公共类型）。其余 Custom 都应先问"缺哪个元素"。
