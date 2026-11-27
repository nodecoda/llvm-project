# 23. 第八轮：覆盖审计与语料补账（31 方言 / 1036 op）

> 配套文件：`corpus/*.dialect`（31 个方言，12063 行）；
> [`dialect-grammar.y`](dialect-grammar.y)（**58 条规则 / 140 条产生式**，0 冲突）；
> 新增工具 [`tools/dialect-coverage.py`](tools/dialect-coverage.py)。
>
> 与前七轮的关系：前七轮是加法（新方言 → 新产生式），这一轮先做减法：
> 把三个"恒为真"的检查改成真检查，用它们量出语料自己的欠账，再补账。
> 结论：新增 3 个方言，补 12 个方言的缺口（258 个 op），文法只多一条产生式。

## 0. 为什么换方向

第七轮结束时，"逃逸点受约束"已经落到文字上（19 章七条规则）。但规则只有在
**检查会失败**时才有约束力。本轮把三条检查逐个验证它们能不能失败，结果是三条
全都不会失败：

| 检查 | 声称 | 实测 |
| --- | --- | --- |
| `dead declarations` | 有声明没被使用就报 | 恒为 `none`（方言 body 把每个名字都数了一次） |
| 类型名解析（R7） | 本语料声明 > 内建 > 野外 | 全局解析，同名声明互相掩盖（第七轮已修） |
| R7 覆盖 `constraint` 右侧 | 所有类型引用 | `;` 结尾的声明 body 为空，右侧从未被扫 |

检查恒真比没有检查更糟：它给出"已验证"的错觉。所以这一轮的第一件事是让它们
能失败；第二件事是把它们第一次失败的结果全部记账。

## 1. 三个新方言

| 方言 | 上游具体 op | 语料 op | 检验点 |
| --- | --- | --- | --- |
| `arm_neon` | 6 | 7 | 打印助记符可以**不是标识符**（`2d.sdot`） |
| `arm_sme` | 63 | 68 | intrinsic 族 43 个 op 全部没有自定义格式：`Generic` 也是可数通道 |
| `ml_program` | 11 | 11 | 符号表 + 两种 region 种类（SSACFG / Graph） |

`arm_sme` 是"结构极简、数量极大"的极端：25 个核心 op 用 15 个 schema 收敛，
43 个 intrinsic 只有类型域与立即数，文本完全由 generic 形式给出。语料里它们
逐条写 `Assembly Generic;`——不写就等于把"没有格式"藏起来。

`ml_program` 暴露的是 region 的**种类**：`func` 的 region 是 SSACFG，`subgraph`
的是 Graph，块数约束（`single_block`）说不出这个区别。这个词没有进核心表，
记在文件头的 GAP 里——一个只在两处出现、且没有第二个方言独立见证的概念，
按 R5 还不该升级。

## 2. 唯一的新产生式：带引号的名字

`arm_neon` 的 2d 变体打印成 `2d.sdot`（`ArmNeon.td:70`，`"2d." # mnemonic`）。
`name` 原先只接受 `IDENT`，而标识符不能以数字开头。

两条路：

| 方案 | 结果 |
| --- | --- |
| 语料里改写成 `sdot.2d` | 语言记录了一个**不存在**的拼写，语料从此不可信 |
| 允许名字带引号 | 语言能逐字记录打印名，`2d.sdot` 原样保留 |

选了后者：

```yacc
name
  : IDENT
  | STRING        /* 打印名不是合法标识符时，逐字记录 */
  | decl_keyword
  ;
```

`STRING` 只出现在名字位置（`op`/`schema`/`type_ref`/参数名/绑定名），与 `Doc`
的字符串字面量不冲突：`Doc` 由 `KW_DOC` 引导，LR(1) 能分辨。bison 0 冲突，
31 个语料全部 `parse OK`。

代价是工具要跟上：`tools/dialect-corpus.py` 的实体扫描与
`tools/dialect-coverage.py` 的 op 抽取都要接受 `op "2d.sdot"` 这种写法。

**这一条产生式的来源值得记住**：它不是设计出来的，是一个真实方言的打印名
把文法逼到墙角逼出来的。前七轮的 346 个 op 只换来 2 条文本产生式，这一轮的
7 个 op 换来 1 条名字产生式——密度高，是因为缺口在"名字"这个所有方言都用的
位置上。

## 3. 三个恒真检查的修复

### 3.1 死声明检查

原实现把 `dialect` 实体也算进"使用计数"，而 dialect 的 body 就是整段文本，
于是每个声明名至少被自己出现的那一行数到一次，`unused` 恒为空。

改成跳过 dialect 容器、并剥掉声明自身的名字一次后，第一次运行就报出 4 个：

| 声明 | 方言 | 真实情况 | 处置 |
| --- | --- | --- | --- |
| `enum BlockSize` | tosa | 上游被 6 个 block-scaled op 使用，而语料缺这 6 个 op | 补 op 时一起补回 |
| `enum BlockShape` | tosa | 上游零使用（只作 `!tosa.block_scaled` 类型参数） | 删除，改为注释 |
| `enum IteratorType` | linalg | 上游属性类型写作 `IteratorType`，语料写成了属性名 `iterator_type` | 改成枚举名 |
| `enum AtomicBinOp` | ptr | 本树只被内存空间**接口**的钩子使用，没有 op 承载 | 删除，概念留在注释 |

四个都不是笔误，而是三种不同的失真：**缺 op**、**上游本来就没有**、
**命名不一致**。检查从"恒真"变成"会失败"之后，三种都能被区分出来。

### 3.2 R7 的两处缺口

- **全局解析**（第七轮发现，`b6ef026` 已修）：`amdgpu` 声明 `constraint scalar`
  后，`tosa` 的 `scalar` 从外部名报告里消失。现在按"本方言 > 内建 > 其它方言"
  三层解析，第三层单独列出；`tosa` 的 `scalar` 回来了。
- **`;` 结尾声明的右侧**：`constraint x = a | b;` 的实体 body 为空，右侧在
  header 里，R7 只扫 body，所以 `constraint mx_scale = f8e8m0fnu;` 里的
  `f8e8m0fnu` 从未被检查。修好后补了三个内建浮点格式
  （`f8e8m0fnu`、`f6e2m3fn`、`f6e3m2fn`），并把"任意位宽整型/浮点"定为内建规则
  （`i\d+` / `f\d+` / `bf\d+`）——MLIR 的 `i14`、`i128` 都是真实类型，
  不该按方言逐个声明。

### 3.3 内建类型规则的收紧

第八轮开始时 `BUILTIN_TYPES` 是一个手写清单，出现过的漏洞：

| 名字 | 真实身份 | 修法 |
| --- | --- | --- |
| `i14` / `i128` | 任意位宽整型 | 规则 `i\d+` |
| `f8e8m0fnu` / `f6e2m3fn` / `f6e3m2fn` | MX / FP6 浮点格式 | 加入清单 + 扫 constraint 右侧 |

手写清单永远会漏；**能用规则表达的就不用清单**。

## 4. 新工具：覆盖审计

### 4.1 为什么需要

19 章 R1 只保证"**写进语料的 op** 都有文本通道"，不保证"上游的 op 都写进了
语料"。两者差得很远：`complex` 语料在第八轮之前只有 1 个 `attr_def`、0 个 op，
却在统计表里是一名"已覆盖方言"。

缺 op 的代价不是数字难看，而是**这个词法/语法从未被该方言的构造测试过**。

### 4.2 设计

`tools/dialect-coverage.py`：

- **显式映射**：47 个方言 → TD 文件列表。不按目录猜，因为 transform op 住在
  别的方言目录里（`GPU/TransformOps/...` 的 op 属于 `transform` 方言）。
- **记录筛选**：只有名字或基类以 `Op` 结尾、且助记符全小写的 `def`/`defm`
  才算 op——enum case、interface、typedef 都是同一种 `def X : Y<"...">` 形状。
- **末段对齐**：intrinsic 族在语料里写 `intr.ld1b.horiz`，上游助记符是
  `ld1b.horiz`；按末段比较，避免把同一 op 记成"缺失 + 多余"。
- **退出码**：任何方言 missing 非空就返回 1，可以直接当门禁。

### 4.3 第一次运行：258 个 op 的欠账

| 方言 | 上游 | 语料 | 缺 | 方言 | 上游 | 语料 | 缺 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| tosa | 103 | 36 | **67** | affine | 16 | 11 | 5 |
| gpu | 68 | 18 | **49** | emitc | 58 | 55 | 3 |
| math | 46 | 15 | **31** | linalg | 17 | 12 | 8 |
| complex | 29 | 0 | **29** | tensor | 21 | 12 | 9 |
| vector | 39 | 20 | **18** | index | 26 | 16 | 10 |
| pdl | 15 | 0 | **15** | memref | 32 | 18 | **14** |

合计 258 个缺失 op、12 个方言。其余 19 个方言 missing = 0。

值得注意的分布：欠账最大的 `tosa`、`gpu`、`complex`、`pdl` 恰好都是"早期轮次
只做了类型层或只做了骨架"的方言。覆盖审计把这些**历史欠账**一次暴露，
而不是让它们继续以"已覆盖"的身份留在统计表里。

## 5. 补账：692 → 1036

| 方言 | 之前 | 之后 | 新增 | 说明 |
| --- | --- | --- | --- | --- |
| tosa | 36 | 103 | +67 | shape 族 16、block-scaled 8、elementwise/activation 18、结构化 25 |
| gpu | 18 | 68 | +50 | 稀疏 SpMM/SDDMM 24、subgroup_mma 12、基础 14 |
| math | 15 | 46 | +31 | 一元/二元/三元/谓词/整数五族 |
| complex | 0 | 29 | +29 | 3 个 schema 收敛 21 个同型算子 |
| vector | 20 | 37 | +17 | 切片/流压缩/索引访存/scalable |
| pdl | 0 | 15 | +15 | 符号表 + 两种 region 骨架 |
| memref | 18 | 32 | +14 | 视图、DMA、原子、元数据 |
| index | 16 | 26 | +10 | s/u 后缀变体全部复用 `index.Binary` |
| tensor | 12 | 21 | +9 | 切片/形状重组/gather-scatter |
| linalg | 12 | 20 | +8 | 4 个命名算符 + softmax + 3 个 winograd |
| affine | 11 | 16 | +5 | dma、vector 访存、linearize |
| emitc | 55 | 58 | +3 | class/field/get_field |

加上三个新方言（7 + 68 + 11 = 86），本轮共 +344 op，语料从 692 到 1036。

补账里没有出现新产生式，但出现了三处**建模判断**：

1. **`tosa.block_size` 是必选属性**，不是可选：上游 `Tosa_BlockSizeAttr` 没有
   默认值。任务描述里的 `optional` 是错的，以 TD 为准。
2. **`memref.generic_atomic_rmw` 的 region** 是 `single_block` +
   `implicit_terminator(atomic_yield)` + `no_region_args`——三个核心事实刚好够，
   不需要新词。
3. **`linalg.softmax` 在本树没有 region**（`input/output/dimension` 三个字段），
   所以没有按"带 combiner region"建模。

## 6. 逃逸点与事实的反面案例

补账跑完，另起一个只读的角色做事实核对：把语料里每个 `Operands`/`Attributes`
绑定、每条 `Properties` 事实、每个 `Assembly Custom` 与上游 TD 逐条对照。
抓到 **6 处 HARD**（4 处本轮新增、2 处既有）与 10 处 IMPRECISE。

### 6.1 事实没有依据（HARD）

| 位置 | 写法 | 上游事实 | 处置 |
| --- | --- | --- | --- |
| `pdl.pattern` | `symbol_table` | 只有 `Symbol`/`SymbolName`/`SymbolVisibility` | 删除 |
| `gpu.launch_func` | `recursive_effects` | `MemoryEffects<[MemRead,MemWrite,MemAlloc]>`，无 `RecursiveMemoryEffects` | 删除 |
| `gpu.subgroup_broadcast` | `speculatable` | `ConditionallySpeculatable`，`first_active_lane` 模式不可推测 | 删除（条件性没有词） |
| `memref.realloc` | `memory_effect(read/write)` | 只有 source 的 `MemFree` 与 result 的 `MemAlloc` | 只留 `allocate`；`free` 记 GAP |

一条通则：**事实必须有 trait / interface / verifier 之一作依据**，语义上"显然
成立"不算。`same_type(matrix, res)` 这类在 TD 里没有 `AllTypesMatch` 的关系，
降级成 Doc 里的一句话。

### 6.2 部件写错子句（HARD）

| 位置 | 错误 | 上游 |
| --- | --- | --- |
| `memref.atomic_rmw` | `kind` 写在 Operands | `AtomicRMWKindAttr:$kind` 是属性 |
| `gpu.spmm_buffer_size` | `operand_segments_by_attribute` | 上游是 `AttrSizedResultSegments`（结果分段） |

第二处暴露一个词表命名问题：`operand_segments_by_attribute` 的名字把"按属性
分段"绑死在操作数上。正确做法是把核心词改成中性的
`segments_by_attribute`，或补 `result_segments_by_attribute`；本轮先不写事实，
记进 GAP——**宁可缺，不可错**。

### 6.3 臆造部件与重复子句

| 位置 | 问题 | 处置 |
| --- | --- | --- |
| `memref.prefetch` | 臆造 `cache_type`；`data`/`instr` 是打印字面量，不是属性 | 删除，改名 `locality_hint` |
| `tosa` `schema Gather`/`Scatter` | 同一 schema 里写了两组 `Operands`/`Results`（较松的那组与 TD 冲突） | 删掉重复组 |

重复子句是最隐蔽的一类：语法合法、审计不报，合并语义时第二组会覆盖第一组。

### 6.4 逃逸点滥用（HARD）

| 位置 | 写法 | 上游 | 处置 |
| --- | --- | --- | --- |
| `pdl.replace` / `apply_native_rewrite` | `custom<...>` | declarative `assemblyFormat` | 改回块格式 |
| `gpu.launch_func` | `Assembly Custom;` | declarative `assemblyFormat` | 改回块格式（`ref(...)` 一处缺口留注释） |

`gpu.launch_func` 的格式有 10 个元素，写回块格式后只剩
`custom<LaunchDimType>(..., ref(cluster_size_x), ...)` 一处写不出——这正是
19 章 §6 记录的 `ref` 缺口，不是新的缺口。**能用块格式写出来的就必须写出来**。

### 6.5 命名失真

`vector` 的 `contraction` 改为上游打印助记符 `contract`。声明名 = 打印名，
这一条在第八轮之前只是约定，现在由覆盖审计（集合对差）自动执行。

## 7. 共享名字空间的第二个受害者：schema 名

R7 修的是**类型名**的可见性。补账时撞到同一问题的另一面：schema 名也是
**全局**解析的。

- `arm_sme` 与 `tensor` 都想叫 `slice_access`；
- `tensor` 与 `vector` 都想叫 `shape_manipulation` / `gather_scatter`。

工具的一层解析会把它们当成同一个 schema，于是 `from` 解析到错误的部件集合，
产生假的 dangling。现在的处置是**在语料层避免重名**（改成
`slice_spec`/`shape_reassociation`/`indexed_memory`/`stream_compaction`），
但这只是绕开，不是修好。第九轮的工单：schema 解析也应该"本方言优先"，
与 R7 同一套三层规则。

## 8. 预算与趋势

| 指标 | 第七轮 | 第八轮 | 红线 |
| --- | --- | --- | --- |
| 方言 | 28 | **31** | — |
| op | 692 | **1036** | — |
| 行数 | 8509 | **12063** | — |
| 产生式 | 138 | **140** | — |
| `Assembly Custom` 占 op | 75/692 = 11% | 82/1036 = **8%** | 25% |
| `Assembly Generic` | 28 | 71（arm_sve 22 + arm_sme 43 + emitc 4 + 其它 2） | — |
| 方言本地事实 | 17 | **20** | — |
| 单次使用本地事实 | 6/17 = 35% | 7/20 = **35%** | 50% |
| 外部署名 | 12 / 7 方言 | 10 / 5 方言 | 逐条可辨认 |
| 覆盖缺口 | 未度量 | **0**（31/31） | 0 |

趋势表：

| 轮次 | 方言 | 累计 op | 累计产生式 | 本轮新增 op | 本轮新增产生式 |
| --- | --- | --- | --- | --- | --- |
| 21 | 25 | 519 | 138 | 81 | 1（文本通道） |
| 22 | 28 | 692 | 138 | 173 | 0 |
| **23** | **31** | **1036** | **140** | **344** | **2（名字、及其名字位）** |

## 9. 判决

1. **收敛判据要升级**：只说"新方言不需要新产生式"不够，还要说"这个方言的
   op 集合是完整的"。前者证明文法够宽，后者证明证据够全。
2. **检查必须能失败**：本轮三条检查修复前全部恒真。以后新增任何检查，先在
   语料里人为制造一个违规，确认它会红，再合入。
3. **欠账会伪装成覆盖**：`complex`/`pdl` 以 0 op 的身份在统计表里待了四轮。
   统计口径（"方言数"）与证据口径（"op 数"）必须同时报。
4. **名字位是高价值缺口**：344 个 op 只换来 1 条文本产生式，7 个 op 换来 1 条
   名字产生式。缺口的价值取决于它被多少方言共享。
5. **逃逸点的每一次使用都要有判据**：pdl 的两处修正说明，规则写在文档里不够，
   要在评审时逐条问"为什么文法写不出"。

## 10. 下一步

- 剩余 16 个方言：`spirv`（434 def，最大）、`llvm`（521 op）、`rocdl`（约 200）、
  `nvvm`（180）、`nvgpu`、`xegpu`、`vcix`、`xevm`、`acc`、`mpi`、`irdl`、`dlti`、
  `bufferization`、`transform`、`wasmssa`、`x86`。
- 工具工单：schema 本方言优先解析；`Assembly` 里 `enum($attr)` 与
  `custom<Hook>(字符串字面量)` 的缺口（19 章 §6 已记录，等语料见证）。
