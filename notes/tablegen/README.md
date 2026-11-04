# TableGen 源码深潜（学习分支）

本目录是 `learn/tablegen-deep-dive` 分支的学习工作区，目标不是"会用 TableGen"，
而是能在源码层面回答：一个 `.td` 文件如何变成记录、记录如何被解析/求值、
后端如何消费记录并生成 C++，以及遇到错误时该看哪个函数。

基线：`llvm-project` @ `fd64c07f9ae1`（2026-11-04 的 upstream/main），
`llvm-tblgen 24.0.0git`，assertions 构建（`/home/ubuntu/llvm/builds/pr-lane`）。

## 目录

| 文件 | 内容 |
| --- | --- |
| `TABLENGEN-INTERNALS.md` | 主文档：架构、源码地图、类型/求值模型、后端、性能、20 条专家级 gotcha |
| `EXPERIMENTS.md` | 实验索引：每个 `.td` 证明了什么，附实测输出 |
| `experiments/01_core.td` | class/def/defm/multiclass/foreach/defset/匿名记录/名字改写 |
| `experiments/02_types.td` | Init/RecTy 类型系统与转换边界 |
| `experiments/03_ops.td` | 62 个 `!` 运算符的实测语义 |
| `experiments/04_preproc.td` | `#ifdef/#define/-D/-E/include` 与 depfile |
| `experiments/errors/*.td` | 24 个负例：一份"错误信息 → 源码位置"对照表 |
| `experiments/run.sh` | 一键复现全部输出到 `out/` |
| `out/` | 实测输出（`-print-records`、错误矩阵、耗时、study report） |

## 30 分钟路线

```bash
source /home/ubuntu/llvm/toolset/env.sh
llvm-use pr-lane                       # 或直接使用 $LLVM_HOME/builds/pr-lane/bin
notes/tablegen/experiments/run.sh       # 重新生成 out/
sed -n '1,120p' notes/tablegen/TABLENGEN-INTERNALS.md
```

然后按 `TABLENGEN-INTERNALS.md` §12 的阅读顺序读源码；每读一个机制，
回到 `experiments/` 找对应实验改一行、跑一遍、观察输出变化。

## 关键结论（先记住）

1. **两阶段**：解析期只做能做的常量折叠；引用字段/模板参数/循环变量的表达式
   留成 `VarInit`/算子节点，在记录 finalize 时做 final resolve。
2. **两个命名空间**：class/def（`RecordKeeper::Classes/Defs`）与 global
   （`defvar`/`defset` 进入 `ExtraGlobals`）。
3. **一个名字两种含义**：`NAME` 是隐式模板参数，`defm` 用前缀拼接生成记录名。
4. **类型是宽松的**：`typeIsConvertibleTo` 与 `typeIsA` 不同；`bits<N>` 不能隐式加宽，
   但 `int`/`bits`/`bit` 之间大量可转换。
5. **`defset` 不是 SetTheory**：前者是解析期的有序 list，后者是后端 C++ 的有序集合引擎。
6. **后端是注册表**：`-gen-*` 是 `TableGen::Emitter::Opt` 注册的字面 cl::opt，
   没有旧式大 switch（clang 的 tblgen 仍是 switch）。

## 学习产出

- `llvm/utils/TableGen/StudyEmitter.cpp`：新增的 `-gen-study` 后端（131 行），
  用真实 API 输出类层级、字段类型直方图、直接派生计数、匿名记录样例。
  这是"能扩展它"的证据；构建只需 2 条边、约 3 秒。
