# MLIR 源码分析：文本解析器与 TableGen 声明层

本目录有两个主题：

- **A. 文本解析**（01–09）：`mlir/lib/AsmParser/` 如何把 MLIR 文本变成内存中的 IR。
- **B. 声明层**（10–23）：`mlir/**/*.td` 如何通过 `include` 组成 ODS 依赖图，
  这张图如何经 `mlir-tblgen` 变成 `.inc` 再进入 C++，寄生在 TD 语法里的
  ODS 语义结构是什么样子（13 章给出两门可编译的语义文法），如果只描述
  方言领域本身、不描述代码生成这门定义语言应该长什么样（14 章），以及
  把真实方言当成源程序、从语料反推出可编译文法（15 章用 6 个方言归纳，
  16 章用 gpu/linalg/tensor 扩充到 9 个并加入谓词组合，17 章再用 5 个
  形状差异更大的方言检验文法是否收敛，18 章换问题：ODS 装不下的概念
  落在 C++ 里，它还是不是领域事实，19 章把"说不出来时往哪儿放"收敛成
  五种受约束的逃逸点，20 章用 emitc/sparse_tensor 压测这套规则，并用
  单次事实复审把本地词表从 26 压回 19，21 章再用 ub/ptr/shard/arm_sve
  证明 81 个新 op 可以只换来 1 个核心词、0 条新产生式，22 章再用
  amdgpu/openmp/smt 把规模推到 173 个 op：最复杂的 openmp 用了 0 个本地词，
  23 章换方向：先把"死声明/类型名/覆盖"三条恒真的检查修成会失败的检查，
  再用覆盖审计量出 258 个缺失 op 并补账，共 31 个方言、1036 个 op，
  文法只多一条产生式：带引号的名字；24 章给五种逃逸点发许可证：断言必须与
  上游 TD 一致，说出不来的构造用 `Gap(name)` 记成登记过的债务，检查第一次
  跑就抓到 16 处假 `Custom` 和 1 个拼错的钩子名）。此后又用 bufferization、
  vcix、xevm、x86、xegpu、nvgpu、mpi、irdl 八个方言把语料推到 39 个方言、
  1171 个 op；`prop_dict` 因 XeGPU/NVGPU/XeVM 共 23 个 op 的证据而从缺口
  升级成 `A_PROP_DICT` 产生式，这是"文法跟不上语料才加产生式"的正例。
  24 章随后把逃逸点从"许可证"推进到"契约"：语法层窄化形状（`ref` 不嵌套、
  `else` 只打字符串、`oilist` 分隔符显式），审计层逐位比对钩子实参（L3b），
  缺口登记成有期限、可销账的债。三条缺口在同一轮被三条产生式关掉，
  语料到 50 个方言、2777 个 op、0 个 open 缺口。

- 分析基线：worktree `/home/ubuntu/llvm/trees/pr-ctpop`，分支 `research/mlir`。
  01–09 基于 `5f7f36636abe`；10–24 基于 `53a80edfeb2e`（2026-11-27）。
- 所有 `file:line` 引用均以对应提交为准；上游演进后行号可能漂移，函数名相对稳定。
- 01–09 的范围以 `mlir/lib/AsmParser/` 为中心；为解释边界，少量引用
  `mlir/include/mlir/Parser/Parser.h`、`mlir/include/mlir/AsmParser/*`、
  `mlir/include/mlir/IR/OpImplementation.h` 与 `mlir/lib/Parser/Parser.cpp`。
- 10–24 的范围是 `mlir/include`、`mlir/lib`、`mlir/python` 下的 412 个 `.td`
  （不含 `mlir/test`、`mlir/examples`、`mlir/unittests`）；14 章的样例在
  `sketch/`，15–24 章的语料在 `corpus/`，v1 归档在 `design-first-v1/`。

## 章节

| 章节 | 内容 |
| --- | --- |
| [01-overview.md](01-overview.md) | 总体结构、模块划分、数据流、公共入口、设计原则 |
| [02-lexer.md](02-lexer.md) | 词法：Token 模型、TokenKinds、lexToken 状态机、各类字面量 |
| [03-parser-core.md](03-parser-core.md) | 语法主干：分层、操作/region/block、SSA 作用域与前向引用 |
| [04-types-and-attributes.md](04-types-and-attributes.md) | 类型与属性解析（含 dense 双语法、affine 简述） |
| [05-dialect-and-interfaces.md](05-dialect-and-interfaces.md) | `#`/`!` 扩展语法、dialect hook 时序、OpAsmParser 桥接 |
| [06-affine-and-location.md](06-affine-and-location.md) | 仿射表达式/映射/整数集、location 语法与延迟别名 |
| [07-tooling-and-diagnostics.md](07-tooling-and-diagnostics.md) | AsmParserState、代码补全、诊断、资源 file metadata |
| [08-walkthrough.md](08-walkthrough.md) | 端到端走查：一段文本从字节到 IR |
| [09-pitfalls-and-internals.md](09-pitfalls-and-internals.md) | 易错点、内部契约、parser/printer 对称性案例 |
| [10-tablegen-overview.md](10-tablegen-overview.md) | `.td` 依赖图总览：扫描口径、412 节点/1132 边/0 环、六层结构、生成节点 |
| [11-tablegen-dependency-graph.md](11-tablegen-dependency-graph.md) | 图形版：IR 内核图、方言间 21 条边、TransformOps 汇聚、最长 16 跳链 |
| [12-tablegen-dialect-anatomy.md](12-tablegen-dialect-anatomy.md) | 单方言解剖：Arith / Linalg 的文件划分、partition、YAML 生成、checklist |
| [13-ods-semantic-grammar.md](13-ods-semantic-grammar.md) | ODS 语义文法：记录层 `ods.y` + `assemblyFormat` 子语言 `ods-asm-format.y`，勘误与语义约束清单 |
| [14-dialect-domain-model.md](14-dialect-domain-model.md) | 方言定义的领域模型：定义 vs 生成的边界、结构/文本/语义三投影、trait 四分类、约束指称语义、schema refinement |
| [15-corpus-driven-grammar.md](15-corpus-driven-grammar.md) | 语料优先归纳文法：6 个方言源程序、词法三类关键字、子句×实体矩阵、本轮 47 条规则 / 98 条产生式、8 个由语料逼出的修正 |
| [16-corpus-round-two.md](16-corpus-round-two.md) | 第二轮语料：gpu/linalg/tensor、oilist/custom/functional-type/region 注解/软关键字、谓词组合（列表=合取，`\|`=析取）、9 个方言 55 条规则 / 128 条产生式 |
| [17-corpus-round-three.md](17-corpus-round-three.md) | 第三轮语料：memref/vector/affine/math/index，验证收敛（80 个 op 只换来 1 条新规则）、per-region 约束首个见证、`type` 作 custom 实参、14 个方言 56 条规则 / 130 条产生式 |
| [18-corpus-round-four.md](18-corpus-round-four.md) | 第四轮语料：pdl_interp/tosa/shape/async/quant，五个“缺口判决”（ODS 落点、声明名=助记符、trait 列表实参、verifier 领域、推迟项）、属性值域、秩进类型、19 个方言 58 条规则 / 136 条产生式 |
| [19-escape-hatches.md](19-escape-hatches.md) | 逃逸点宪章：五种通道（块/Custom/Generic/事实/类型名）、七条规则（显式、单维、可数、判据、词表、预算、类型名）、核心词表 39 + 方言本地 26（20 章复审后 19）、诚实的无通道缺口清单 |
| [20-corpus-round-five.md](20-corpus-round-five.md) | 第五轮语料：emitc/sparse_tensor，92 个 op 换 0 条产生式、`oilist` 读窄的修正、同名类型/属性空间、字典型属性、预算首次告警与 8 合 7 留的单次事实复审 |
| [22-corpus-round-seven.md](22-corpus-round-seven.md) | 第七轮语料：amdgpu/openmp/smt（173 op），0 条产生式；`speculatable` 升级进核心（40→41）、本地词表 18→17；openmp 70 个子句钩子 + 0 个本地事实；共享名字空间的检查缺陷 |
| [21-corpus-round-six.md](21-corpus-round-six.md) | 第六轮语料：ub/ptr/shard/arm_sve，81 个 op 换 0 条 op 形状产生式；`same_type` 升级进核心（39→40）、本地词表 19→18；`qualified` 第三个见证落地为文本产生式（11 处使用点）；空负载属性、符号非类型、Generic 是契约 |
| [23-corpus-round-eight.md](23-corpus-round-eight.md) | 第八轮：三条恒真检查修成会失败、覆盖审计量出 258 个缺失 op 并补账、带引号的 `name`、schema 名全局解析缺陷 |
| [24-escape-licenses.md](24-escape-licenses.md) | 逃逸点契约：断言与描述分离、五通道许可证、设计规则 E0–E10（封闭集/单通道/可证伪/实参即范围/不组合/有期限/不带语义/家族软名/覆盖收口/守卫可缺失）、范围细则 S1–S10、L0–L12 与 33 例 selftest、三条缺口关闭（`ref`/`oilist<sep>`/`else`）、分方言预算账本 |
| [25-definition-audit.md](25-definition-audit.md) | 定义层契约：四元组（范围/形状/证据/预算）、四通道（块/Custom/Generic/Modeled+脊）、D0–D14、category 封闭词表（rename/cxx-type/cxx-enum/builtin/constraint/borrowed-enum）、符号级见证、modeled 扁脊与 12/4 预算、覆盖与健全性分离（287 声明；367 未声明 = 204 unreferenced + 163 substituted，0 unaccounted） |

## 一页结论

### A. 文本解析

1. **没有独立 AST**。MLIR 解析器是单遍、事件驱动的：读取 token 的同时调用
   `OpBuilder` 直接创建 `Operation`/`Block`/`Region`（`Parser.cpp:2921` 起）。
2. **两层分派**：`Lexer` 只负责切 token；`Parser` 负责内置类型/属性/affine/location；
   `OperationParser` 负责 SSA 名、block、region、操作；`TopLevelOperationParser`
   负责文件级构造（`#alias`、`!alias`、`{-# ... #-}`）。
3. **SSA 名字是延迟绑定的**：`parseOperand` 只记录名字/结果号
   （`UnresolvedOperand`），真正成 `Value` 要等 `resolveOperand` / `resolveSSAUse`；
   未定义的名字先建成 `builtin.unrealized_conversion_cast` 占位值，定义出现后再 RAUW
   （`Parser.cpp:1133`、`:1242`）。
4. **方言是扩展点**：内置关键字在 lexer 里就是专用 token；`!`/`#` 走
   `DialectSymbolParser.cpp` 的 alias/pretty/verbose 三态分派，再把 lexer 指针
   临时重置到 `<...>` 体内，调用 dialect 注册的解析回调（`DialectSymbolParser.cpp:258`、`:305`）。
5. **诊断是“遇到第一个错误就停”**：没有错误恢复；`emitWrongTokenError` 会把
   诊断位置回溯到上一行行尾，避免指向下一个 token（`Parser.cpp:255` 起）。
6. **工具能力旁路**：`AsmParserState` 记录“源码位置 ↔ IR 实体”的映射，
   供 LSP/格式化/补全使用；解析主流程不依赖它（`ParserState.h:81-83`）。

### B. TableGen 声明层

1. **`.td` include 图是 DAG**：412 个文件、1132 条边、0 环，最大深度 16。
   `include` 是词法替换（`llvm/docs/TableGen/ProgRef.md:242`），所以必须能拓扑排序。
2. **脊柱只有 10 个文件**：`Utils → Constraints → CommonTypeConstraints →
   CommonAttrConstraints → AttrTypeBase → Interfaces → OpBase`。`OpBase.td` 被 127 个文件
   直接 include、320 个文件传递依赖（77.7%）。
3. **方言层几乎平坦**：48 个方言只有 21 条“方言→方言”边，最长 4 跳
   （`AMDGPU → LLVMIR → GPU → DLTI`）；复杂度来自横向使用 `mlir/Interfaces`。
4. **`Passes.td` 与 op 定义解耦**：38 个里 32 个只 include `PassBase.td`，深度 1。
5. **三条边只在 CMake 里**：OpenMP/OpenACC 的 clause 定义以 LLVM 的 `OMP.td`/`ACC.td`
   为源，Linalg 的 59 个 named op 由 YAML 生成——`.td` 图上看不到，必须读
   `mlir_tablegen()` 才能发现。
6. **ODS 的实体 kind 由基类链决定**：`isSubClassOf("Op")`、`getAllDerivedDefinitions("Dialect")`
   才是判定依据（`OpGenHelpers.cpp:66-75`、`DialectGen.cpp:402`）；TD 里没有
   "定义 Op" 的语法关键字。
7. **同一个 op 有三种等价写法**：MI 式（`Arguments<...>, Results<...>` 作 mixin）、
   body-let 式（`let arguments = ...`）、family 式（带模板参数的 `class`）。
   13 章把它们统一进一份文法。
8. **ODS 语义文法没有 parser 执行**：`TGParser` 只认 TD 宿主语法，ODS backend 用
   C++ 遍历 `Record` 做语义识别；13 章的 `ods.y` / `ods-asm-format.y` 是"从语义
   反推的规范"，用于审阅与合法性分析，不是可运行的解析器。
9. **方言定义只需描述三件事**：结构（部件）、文本（assembly 双向程序）、语义
   （property/invariant）。`Namespace` / `Class` / `Builders` / `Extra` 回答的是
   "生成的 C++ 长什么样"，不属于定义语言（14 章）。
10. **trait 不是一类东西**：结构角色、代数性质、效应摘要、能力四类混在
    `Traits<[...]>` 里；`Pure` 本身还是 `AlwaysSpeculatable + NoMemoryEffect`
    的合取（`SideEffectInterfaces.td:155`）。
11. **部件间的类型相等是约束，不是 trait**：`SameOperandsAndResultType` /
    `AllTypesMatch` / `TypesMatchWith` 在领域模型里都是 `type_of(x)`，一个构造
    取代三个特殊机制。
12. **文法可以从语料长出来，但不能自动长出来**：6 个真实方言写成 650 行
    源程序后，归纳出 47 条规则 / 98 条产生式，`bison -Wall -Wcounterexamples
    -Werror` 0 冲突、6/6 解析通过、0 悬挂引用；但“哪些构造是同一件事”仍是
    人判断（15 章）。
13. **语料逼出了五个设计错误**：op 的双名字（助记符再起名）、schema 的
    `mnemonic` 参数、后缀 `optional`、Assembly 的 `else`、空列表的两种写法；
    设计先行时它们都“看起来合理”。
14. **`Groups` 的逗号写法会被 LR(1) 拒绝**：改成“一个 `Groups` 子句一个组、
    组内只用 `|`”后 0 冲突，也更符合领域事实——组是命名并集，不是列表。
15. **下一步不是扩文法，而是扩语料**：`gpu`（interface 定义、多 region）与
    `linalg`（partial custom、`oilist`、YAML named op）是下一轮最值得写的
    源程序；文法跟不上才是新产生式的合法来源。
16. **谓词是可组合的逻辑，不是特殊谓词表**：`Properties`/`Implements`/
    region 注解的列表是合取，`|` 是析取；`parent_one_of` 被删除。拼写用
    `|` 而不是 `or`，因为 `or`/`and`/`xor` 是 `gpu.all_reduce` 的合法 case 名
    （16 章）。
17. **约束挂在它约束的实体上**：`single_block` 从 op 属性搬到 region 绑定，
    才能表达「变长 region 组里每个 region 都是单块」（`scf.reduce`）；
    `gpu.launch`/`gpu.func`/`func.func` 是 `AnyRegion`，不能标（16 章）。
18. **软关键字让领域名不被语言绑架**：`gpu.all_reduce` 的属性就叫 `op`，
    所以声明关键字在名字位置降级为标识符（16 章）。
19. **新构造由新方言逼出**：3 个方言换来 10 项扩展，其中 6 项在此前 6 个
    方言里从未出现；`custom<...>`、`oilist`、`functional-type` 都不是猜的
    （16 章）。
20. **收敛的判据是“新方言不需要新产生式”**：5 个方言 80 个 op 只换来 1 条
    新规则，9 类新构造被已有产生式接住；如果每轮都要加规则，说明上一轮的
    抽象抓的是个别方言而非领域共性（17 章）。
21. **per-region 约束从设计直觉变成真实需求**：`affine.if` 的
    `SizedRegion<1>:$thenRegion, AnyRegion:$elseRegion` 是第一个“同一 op 两个
    region 约束不同”的见证，op 级属性无法表达（17 章）。
22. **关键字只在起语法作用的位置是关键字**：`memref.global` 的部件名就叫
    `type`，`type,` 与 `type(` 靠 LR(1) lookahead 区分；软关键字思路在
    Assembly 模式里同样成立（17 章）。
23. **“ODS 装不下”不等于“不是领域事实”**：`quant` 四个类型的文法全在
    `TypeParser.cpp` 里手写，但它们仍是最纯粹的领域事实（存储/表达/比例/
    零点）；`type_def` + `Assembly` 的存在理由就是这一类（18 章）。
24. **很多“文法缺口”是用错了词**：`type_def Token` 照抄了 C++ 类名，
    `int2 = array<i64>` 把长度编码进名字，`all_shapes_match([a, b])` 照抄了
    容器的方括号；换成领域词，缺口就消失（18 章）。
25. **约束挂载点看作用范围**：一个 schema 的所有实例同长就挂绑定注解
    （`pad: array<i64> [cardinality(4)]`），不同实例长度不同就挂 op 事实；
    与 17 章 per-region 约束同一条判据（18 章）。
26. **值可以进类型**：`tensor<number, 4>`、`shape<2>` 比 `Tosa_Tensor4D`、
    `Rank2TosaShape` 更直接——秩是值，不是另一个类型名（18 章）。
27. **定义语言只描述“结构 + 文本 + 可陈述的事实”**：region 入口块参数与
    操作数的解包关系是事实（`entry_args_unwrap`），不是语法位置；ODS 自己
    也没有对应声明（18 章）。
28. **逃逸点必须显式**：三种文本通道（块 / `Custom` / `Generic`）三选一，
    不允许省略；省略是看不见的逃逸，读者无法区分“故意”与“忘了”（19 章 R1）。
29. **逃逸点只覆盖一维**：`Custom`/`Generic` 只覆盖文本，结构仍要声明；
    `custom<Hook>` 只覆盖实参列出的部件；事实只覆盖性质，不得重复
    `Implements`/子句已说过的结构（19 章 R2，本轮删掉 3 个重复事实）。
30. **事实词表受管**：核心 41 + 方言本地 17；同一概念一个词、组合不造新词、
    跨方言借用要升级；预算超线时先怀疑构造，不要先说“这个方言特殊”（19 章）。
31. **读窄比写窄更危险**：`oilist` 的语法一直是“每支 0/1、顺序无关”，15 章起
    被读成“互斥”，语料把反例挪进注释——parse 抓不到，只能对照上游文档（20 章）。
32. **属性层/类型层再厚也不膨胀文法**：92 个新 op 只用到 `type_def`/`attr_def`
    + `Assembly` + `Parameters`，产生式 0 增（20 章）。
33. **同名不同类型空间是真实需求**：`!emitc.opaque<...>` 与 `#emitc.opaque<...>`
    共享助记符、不共享命名空间（20 章）。
34. **预算告警先于失控**：两个方言就把本地事实 8→34；复审不删信息，只换词，
    单次占比 59%→35%（19 章 R6、20 章 §6.1、22 章 §7）。
35. **op 数量与文法大小已经脱钩**：最后 346 个 op（第五至七轮）只换来
    2 条产生式（都是文本通道）、2 个核心词，比例 167:1（21–22 章）。
36. **同一个缺口第三次出现就该改文法**：`qualified(type(x))` 在
    sparse_tensor/emitc/ptr 三处独立出现，已成为产生式，11 处使用点同时落地（21 章）。
37. **“没有类型”和“没有格式”都是领域事实**：`shard.grid` 是符号 op 而非
    `GridType`；`arm_sve` 的 scalable vector 是内建 `vector`，不发明方言类型（21 章）。
38. **Generic 是契约，不是历史遗留**：`arm_sve` 的 23 个 `intr.*` 与 21 个
    用户可见 op 是同一概念的两种文本通道，两者都要显式声明（21 章）。
39. **最复杂的方言用了最少的本地词**：openmp 72 op、70 个子句钩子、135 次
    事实，0 个本地事实——核心 41 词覆盖了子句语义（22 章）。
40. **词表进入归并期**：连续两轮的新词都是别的方言已有的词
    （`same_type`、`speculatable`），升级进核心而不是新增本地词（21–22 章）。
41. **共享名字空间要配“本方言可见”的检查**：跨方言复用是特性，
    同名静默解析会掩盖拼写错误；R7 的判据要改（22 章）。
42. **Custom 的正当理由只剩缺省/派生文本**：`undef`、公共类型、由位宽导出的
    次数；其余 Custom 都应先问“缺哪个元素”（22 章）。

## A 部分：`mlir/lib/AsmParser/` 文件职责

| 文件 | 行数 | 职责 |
| --- | --- | --- |
| `Lexer.cpp/.h` | 450/95 | 把字节流切成 `Token`，编码源码位置 |
| `Token.cpp/.h` + `TokenKinds.def` | 211/135/143 | Token 种类定义与字面量解码 |
| `Parser.h` | 377 | `Parser` 基类：token、类型、属性、location、affine、补全接口 |
| `ParserState.h` | 99 | 解析期全局状态：lexer、当前/上一个 token、符号表、默认 dialect 栈 |
| `Parser.cpp` | 2991 | `OperationParser`/`TopLevelOperationParser` 与公共入口 |
| `AsmParserImpl.h` | 650 | `OpAsmParser`/`DialectAsmParser` 接口的内部适配层 |
| `TypeParser.cpp` | 634 | 内置类型与 `!` 扩展类型 |
| `AttributeParser.cpp` | 1404 | 内置属性、attr-dict、dense/sparse/strided/distinct |
| `DialectSymbolParser.cpp` | 385 | `#`/`!` 的 alias 与 dialect 分派、独立字符串解析 API |
| `AffineParser.cpp` | 821 | `affine_map`/`affine_set`/affine expr |
| `LocationParser.cpp` | 237 | `loc(...)` 各种形式 |
| `AsmParserState.cpp` | 433 | 工具侧源码映射状态的记录与查询 |

## B 部分：产出物

| 文件 | 内容 |
| --- | --- |
| `10-tablegen-overview.md` | 统计口径、六层结构、include 语义、生成节点、结论 |
| `11-tablegen-dependency-graph.md` | 4 张 Mermaid 图 + 度数排行 + 深度分布 |
| `12-tablegen-dialect-anatomy.md` | Arith / Linalg 解剖、新方言 checklist、常见错误 |
| `13-ods-semantic-grammar.md` | ODS 语义文法、与 TD 宿主语法的映射、5 处常见误写、29 条语义约束 |
| `ods.y` | 记录层语义文法（bison，0 冲突）：实体、字段、值、约束表达式 |
| `ods-asm-format.y` | `assemblyFormat` 子语言文法（bison，0 冲突） |
| `14-dialect-domain-model.md` | 领域模型：定义/生成边界、三投影、trait 分类、约束语义、schema refinement、v1 逐项判决 |
| `15-corpus-driven-grammar.md` | 语料优先：方法论、语料统计、见证表、8 个修正、机械核对、与 v1/v2 差异、下一轮语料 |
| `corpus/*.dialect` | 50 个方言源程序：2777 op；50 个方言覆盖缺口为 0（逐字对齐 tblgen），0 个 open 缺口 |
| `dialect-grammar.y` | 从语料归纳的文法（bison，62 条规则 / 156 条产生式，0 冲突，`50/50 parse OK`），含 `ref(...)`、`else`、`oilist<sep>`、`qualified(x)`、`Gap(name)`、空字典默认值 `{}`、`params`/`struct(params)`、`Modeled` |
| `16-corpus-round-two.md` | 第二轮：10 项扩展、谓词组合、region 约束搬家、软关键字、见证表、机械核对 |
| `corpus/gpu.dialect` | gpu：launch/module/func、内建 id/dim、barrier/printf/shuffle/all_reduce/subgroup_reduce/warp（18 op） |
| `corpus/linalg.dialect` | linalg：`Structured` schema 收敛 59 个命名算符 + generic/map/reduce/transpose/broadcast/index/yield + softmax/winograd/pack/unpack（78 op） |
| `corpus/tensor.dialect` | tensor：元素访问/切片/填充/生成，partial custom 与谓词析取（12 op） |
| `17-corpus-round-three.md` | 第三轮：收敛验证、per-region 约束见证、`type` 作 custom 实参、跨方言枚举复用、见证表、机械核对 |
| `corpus/memref.dialect` | memref：alloc/alloca/load/store/subview/reinterpret_cast/view/global/atomic_rmw/prefetch 等（18 op） |
| `corpus/vector.dialect` | vector：transfer_read/write、maskedload/store、broadcast/fma/bitcast、extract/insert、scalable.extract/insert、reduction/contraction、print（39 op） |
| `corpus/affine.dialect` | affine：apply/min/max/for/parallel/if/load/store/prefetch/delinearize_index（11 op） |
| `corpus/math.dialect` | math：15 个 op 收敛为 4 个 schema，复用 arith 的 FastMathFlags/RoundingMode |
| `corpus/index.dialect` | index：全部 operand/result 都是 `index`，10 个二元 op 共用一个 schema（16 op） |
| `18-corpus-round-four.md` | 第四轮：五个缺口判决、quant 的 C++ 落点、属性值域、秩进类型、约束挂载点判据、机械核对 |
| `corpus/pdl_interp.dialect` | pdl_interp：谓词/分支/switch 基本块原语，20 个 `get_*`/`check_*` 由 4 个 schema 覆盖（39 op） |
| `corpus/tosa.dialect` | tosa：属性密集（定长数组/枚举/属性包/符号名），`avg_pool2d` 与 `avg_pool2d_adaptive` 两种窗口形状（36 op） |
| `corpus/shape.dialect` | shape：纯类型层，`witness` 证明值与 `assuming` 单块 region（40 op） |
| `corpus/async.dialect` | async：六个自有类型（token/group/value/coro.*）与“值既是结果又是 region 入口”（29 op） |
| `corpus/quant.dialect` | quant：4 个类型的文法全在 C++ 里，用 `type_def` 描述领域模型并用 `Assembly Modeled;` 承认文本不可审计；3 个 cast（3 op） |
| `19-escape-hatches.md` | 逃逸点宪章：五种通道、七条规则、核心词表 39 + 方言本地 19、无通道缺口清单、预算红线 |
| `20-corpus-round-five.md` | 第五轮：92 op / 0 产生式、oilist 语义修正、`Assembly Generic;` 落地、字典属性、预算告警与单次事实复审 |
| `corpus/emitc.dialect` | emitc：C 类型系统进 MLIR，`type_def` 为主（opaque/array/ptr/lvalue），55 op 只有 4 schema |
| `corpus/sparse_tensor.dialect` | sparse_tensor：`encoding` 为键值字典、`level_type` 为“枚举头 + 两个独立后缀”，37 op |
| `21-corpus-round-six.md` | 第六轮：81 op / 0 产生式、`same_type` 进核心、`qualified` 判决、家族压缩与双通道、空负载属性、趋势表 |
| `corpus/ub.dialect` | ub：毒值与不可达；无参数属性写成 `Assembly { "" }`（2 op） |
| `corpus/ptr.dialect` | ptr：不透明指针只参数化内存空间；2 类型 / 4 枚举 / 13 op |
| `corpus/shard.dialect` | shard：`grid` 是符号 op 而非类型；12 个集合通信共 1 schema（22 op） |
| `corpus/arm_sve.dialect` | arm_sve：44 op 压成 13 schema；21 块格式 + 23 Generic，含唯一的 `intr.bfmmla` 块格式 |
| `22-corpus-round-seven.md` | 第七轮：173 op / 0 产生式、`speculatable` 进核心、openmp 子句极限、共享名字空间缺陷、趋势表 |
| `corpus/amdgpu.dialect` | amdgpu：41 个约束的类型域、11 schema、47 op（含 4 个模板派生），3 Custom（`undef`） |
| `corpus/openmp.dialect` | omp：72 op、24 enum、70 个 `custom<>` 子句钩子、0 本地事实 |
| `corpus/smt.dialect` | smt：6 个 sort 类型、42 op、13 个二元位向量 op 共 1 schema |
| `tools/dialect-corpus.py` | 语料统计与审计：子句×实体矩阵、Assembly 频次、region 注解、悬挂引用、死声明、C++ 泄漏，以及逃逸点三段报告（`escape points` / `text channels` / `fact vocabulary`） |
| `23-corpus-round-eight.md` | 第八轮：三条恒真检查的修复、覆盖审计（258 op 欠账）、补账到 1036 op、带引号的 `name`、pdl 两处逃逸点修正、schema 名全局解析缺陷 |
| `corpus/arm_neon.dialect` | arm_neon：7 op、3 schema；`"2d.sdot"` 用带引号名字保留以数字开头的打印名 |
| `corpus/arm_sme.dialect` | arm_sme：68 op（25 核心 + 43 intrinsic）、15 schema；intrinsic 全部显式 `Assembly Generic;` |
| `corpus/ml_program.dialect` | ml_program：11 op、4 schema；符号表 + SSACFG/Graph 两种 region 种类（种类本身无核心词） |
| `tools/dialect-coverage.py` | 覆盖审计：50 个方言 → 聚合 TD 入口的显式映射；上游取自 `llvm-tblgen --dump-json` 的 `opName`，与语料 `op` 集合**逐字相等**（不做后缀对齐）；missing/extra 非空则退出码 1 |
| `tools/dialect-lex.c` | 参考词法器（normal / assembly 两模式，含 `Gap` 关键字） |
| `tools/escape-audit.py` | 逃逸点许可证审计：L0–L12 + 规范化 N1（`enum($x)`）、tblgen 逐 op 证据、预算硬上限；L3d 查本地标签的子句关键字，L4 的 scope 是封闭词表，L10 查块覆盖与家族软名，L11 查守卫可缺失，L12 拒绝定义层指令；缺 `LLVM_TBLGEN` 或查不到上游记录即失败 |
| `tools/def-audit.py` | 定义层审计：D0–D14；通道与上游一致、参数全覆盖、enum 逐项一致、`params` 全量规则、modeled 符号级见证与扁平脊、12/4 预算、定义块钩子上游存在；`--coverage` 把未声明记录分成 unreferenced / substituted / unaccounted 三类（unaccounted 非空、或有 substituted 却无头部 `/// 定义层范围:` 注记即失败），alias 目标与 ledger 行不计欠账，`--selftest` 23 例 |
| `tools/def-exceptions.tsv` | 定义层逃逸账本：7 列（dialect/kind/corpus/upstream/witness/category/reason），34 行 = 3 rename + 31 modeled（20 cxx-type、4 borrowed-enum、3 builtin、2 cxx-enum、2 constraint） |
| `tools/fact-glossary.tsv` | 事实词表：41 核心 + 20 本地（含 owner 与释义），新词未登记即失败 |
| `tools/gap-registry.tsv` | 表达力缺口登记：4 个名字 / 9 个 op，见证集合必须与语料一致 |
| `tools/hook-registry.tsv` | 上游找不到同名 `custom<>` 的本地钩子标签及理由 |
| `sketch/arith.dialect` | 领域描述样例：3 约束 + 4 schema + 8 op，无 C++/namespace/class |
| `design-first-v1/` | 归档：先定文法再凑样例的 v1（`dialect-dsl.y` + 5 个 `.mdl` + 词法器），仅作对照 |
| `tools/td-dependency-graph.py` | 扫描脚本：统计 + `--mermaid` + `--json`，可复现全部数字 |

> 表中 `corpus/*.dialect` 一行的括号内数字是**该轮引入时**的规模；当前规模以
> `corpus/*.dialect` 合计行、23 章 §8 与 `python3 tools/dialect-corpus.py` 为准。
