# MLIR 文本解析器（AsmParser）源码分析

本目录分析 `mlir/lib/AsmParser/` 的实现：MLIR 文本格式如何被词法分析、语法分析，
并直接构造出内存中的 IR。

- 分析基线：worktree `/home/ubuntu/llvm/trees/pr-ctpop`，分支 `research/mlir`，
  HEAD `5f7f36636abe`（2026-11-27，基于 `upstream/main`）。
- 所有 `file:line` 引用均以该提交为准；上游演进后行号可能漂移，函数名相对稳定。
- 范围以 `mlir/lib/AsmParser/` 为中心；为解释边界，少量引用
  `mlir/include/mlir/Parser/Parser.h`、`mlir/include/mlir/AsmParser/*`、
  `mlir/include/mlir/IR/OpImplementation.h` 与 `mlir/lib/Parser/Parser.cpp`。

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

## 一页结论

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

## 目录文件职责

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
