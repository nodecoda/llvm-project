# legalizer-table —— 表化合法性算法这条线的本地笔记

这个目录放 `sync/table-classify` 这条线的**本地研究产物**（设计分析、判据、
复现记录），不进上游提交序列；与代码相关的改动仍按 `[GlobalISel] ...` 的形状
单独提交。

目录结构：

- `caching-rule-vs-answer.md` —— 表应该缓存什么：规则号还是答案。以
  `a076fbbf9e72` / `f3acadafe20d` / `b2b2b62815ba` 为证据，说明"商必须细于
  被缓存的函数"，以及 `i128`/`s128` 这类"相等但拼写不同"的盲点。

代码与工具的位置：

- 生成器：`llvm/utils/TableGen/LegalizerInfoEmitter.cpp`（`-gen-legalizer-table`）
- 运行时：`llvm/lib/Target/X86/GISel/X86LegalizerTable.{h,cpp}` /
  `llvm/lib/Target/AArch64/GISel/AArch64LegalizerTable.{h,cpp}`
- 测量脚本：`llvm/utils/gisel-table-bench/`（`bench.py`、`gen_ir.py`）
- 门禁：`llvm/test/CodeGen/X86/GlobalISel/table-check.ll`、
  `table-addrspace.ll`、`table-check-32bit.ll`

版本：写于 `sync/table-classify` @ `b2b2b62815ba`（2026-11-01）。
