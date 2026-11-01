# legalizer-table —— 表化合法性算法这条线的本地笔记

这个目录放 `sync/table-classify` 这条线的**本地研究产物**（设计分析、判据、
复现记录），不进上游提交序列；与代码相关的改动仍按 `[GlobalISel] ...` 的形状
单独提交。

目录结构：

- `caching-rule-vs-answer.md` —— 表应该缓存什么：规则号还是答案。以
  `a076fbbf9e72` / `f3acadafe20d` / `b2b2b62815ba` 为证据，说明"商必须细于
  被缓存的函数"，以及 `i128`/`s128` 这类"相等但拼写不同"的盲点。
  技术版在 §1–§10，人话版和五个例子在 §11。
- `generator-walkthrough.md` —— `-gen-legalizer-table` 的实现走读：三个阶段
  （共享校验 / Core IR / 键划分与描述符）、真实数字、运行时怎么消费，
  以及 4.1 每条要求对应的正确性不变量、破坏方式和测试。
- `regression-test-design.md` —— 表化 legalizer 的回归测试集设计：形式化
  oracle（soundness / fallback / key adequacy / coverage / subtarget /
  generator consistency）、L0–L5 分层、测试矩阵、变异负控和 P0 清单。
- `current-legalizer-testing.md` —— 现状调查：GlobalISel legalizer C++
  目前由哪些单测、MIR/lit、TableGen 负例和 `LegalizerInfo::verify()`
  覆盖；含 X86 的规则集/opcode/特征覆盖量化，以及相对表化功能仍然缺什么。

代码与工具的位置：

- 生成器：`llvm/utils/TableGen/LegalizerInfoEmitter.cpp`（`-gen-legalizer-table`）
- 运行时：`llvm/lib/Target/X86/GISel/X86LegalizerTable.{h,cpp}` /
  `llvm/lib/Target/AArch64/GISel/AArch64LegalizerTable.{h,cpp}`
- 测量脚本：`llvm/utils/gisel-table-bench/`（`bench.py`、`gen_ir.py`）
- 门禁：`llvm/test/CodeGen/X86/GlobalISel/table-check.ll`、
  `table-addrspace.ll`、`table-check-32bit.ll`、`table-extended-llt.ll`。
  X86 答案缓存已移除：cell 只存规则号，答案按真实 query 回放，
  `table-extended-llt.ll` 的负控 RUN 与 `cached` 统计一并删掉。
- extended LLT 的测试开关：`-x86-gisel-extended-llt`（hidden，默认关；
  只到 `-stop-after=legalizer`，X86 的选择器不支持）

版本：写于 `sync/table-classify` @ `b2b2b62815ba`（2026-11-01）；
第 6、9、10 节在门控落地后更新于 `233c198b5c10`；
`generator-walkthrough.md` 写于 `bcd51617a6e3`；
`regression-test-design.md` / `current-legalizer-testing.md` 写于
2026-11-02（同日移除 X86 答案缓存）。
