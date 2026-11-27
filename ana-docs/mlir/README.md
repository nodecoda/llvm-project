# MLIR 源码分析：文本解析器与 TableGen 声明层

本目录有两个主题：

- **A. 文本解析**（01–09）：`mlir/lib/AsmParser/` 如何把 MLIR 文本变成内存中的 IR。
- **B. 声明层依赖**（10–12）：`mlir/**/*.td` 如何通过 `include` 组成 ODS 依赖图，
  以及这张图如何经 `mlir-tblgen` 变成 `.inc` 再进入 C++。

- 分析基线：worktree `/home/ubuntu/llvm/trees/pr-ctpop`，分支 `research/mlir`。
  01–09 基于 `5f7f36636abe`；10–12 基于 `53a80edfeb2e`（2026-11-27，含前一提交的文档）。
- 所有 `file:line` 引用均以对应提交为准；上游演进后行号可能漂移，函数名相对稳定。
- 01–09 的范围以 `mlir/lib/AsmParser/` 为中心；为解释边界，少量引用
  `mlir/include/mlir/Parser/Parser.h`、`mlir/include/mlir/AsmParser/*`、
  `mlir/include/mlir/IR/OpImplementation.h` 与 `mlir/lib/Parser/Parser.cpp`。
- 10–12 的范围是 `mlir/include`、`mlir/lib`、`mlir/python` 下的 412 个 `.td`
  （不含 `mlir/test`、`mlir/examples`、`mlir/unittests`）。

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
| `tools/td-dependency-graph.py` | 扫描脚本：统计 + `--mermaid` + `--json`，可复现全部数字 |
