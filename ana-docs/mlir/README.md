# MLIR 源码分析：文本解析器与 TableGen 声明层

本目录有两个主题：

- **A. 文本解析**（01–09）：`mlir/lib/AsmParser/` 如何把 MLIR 文本变成内存中的 IR。
- **B. 声明层**（10–15）：`mlir/**/*.td` 如何通过 `include` 组成 ODS 依赖图，
  这张图如何经 `mlir-tblgen` 变成 `.inc` 再进入 C++，寄生在 TD 语法里的
  ODS 语义结构是什么样子（13 章给出两门可编译的语义文法），如果只描述
  方言领域本身、不描述代码生成这门定义语言应该长什么样（14 章），以及
  把真实方言当成源程序、从语料反推出可编译文法（15 章）。

- 分析基线：worktree `/home/ubuntu/llvm/trees/pr-ctpop`，分支 `research/mlir`。
  01–09 基于 `5f7f36636abe`；10–15 基于 `53a80edfeb2e`（2026-11-27）。
- 所有 `file:line` 引用均以对应提交为准；上游演进后行号可能漂移，函数名相对稳定。
- 01–09 的范围以 `mlir/lib/AsmParser/` 为中心；为解释边界，少量引用
  `mlir/include/mlir/Parser/Parser.h`、`mlir/include/mlir/AsmParser/*`、
  `mlir/include/mlir/IR/OpImplementation.h` 与 `mlir/lib/Parser/Parser.cpp`。
- 10–15 的范围是 `mlir/include`、`mlir/lib`、`mlir/python` 下的 412 个 `.td`
  （不含 `mlir/test`、`mlir/examples`、`mlir/unittests`）；14 章的样例在
  `sketch/`，15 章的语料在 `corpus/`，v1 归档在 `design-first-v1/`。

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
| [15-corpus-driven-grammar.md](15-corpus-driven-grammar.md) | 语料优先归纳文法：6 个方言源程序、词法三类关键字、子句×实体矩阵、47 条规则 / 98 条产生式、8 个由语料逼出的修正 |

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
| `corpus/*.dialect` | 6 个方言源程序（arith/scf/func/cf/complex/pdl，650 行）：77 op、13 schema、5 enum、1 attr_def、4 type_def |
| `dialect-grammar.y` | 从语料归纳的文法（bison，47 条规则 / 98 条产生式，0 冲突） |
| `tools/dialect-corpus.py` | 语料统计与审计：子句×实体矩阵、Assembly 频次、悬挂引用、死声明、C++ 泄漏检查 |
| `tools/dialect-lex.c` | 参考词法器（normal / assembly 两模式），用于 `6/6 parse OK` |
| `sketch/arith.dialect` | 领域描述样例：3 约束 + 4 schema + 8 op，无 C++/namespace/class |
| `design-first-v1/` | 归档：先定文法再凑样例的 v1（`dialect-dsl.y` + 5 个 `.mdl` + 词法器），仅作对照 |
| `tools/td-dependency-graph.py` | 扫描脚本：统计 + `--mermaid` + `--json`，可复现全部数字 |
