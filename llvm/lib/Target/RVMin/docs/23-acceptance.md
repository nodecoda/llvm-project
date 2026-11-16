# 第 23 章：下篇验收

## 目标

下篇 15–22 章各自有一道门禁，本章把它们收拢成一张可以逐条复现的验收表，
并回答两个问题：

1. **下篇承诺的东西，真的都做到了吗？**（P2-AC1–10，每条给命令与当前证据）
2. **哪些东西明确没做？**（未做清单，避免“看起来像完整后端”的误解）

本章不新增代码，只新增证据；`rvmin-ch23` 之后第一课冻结，第二课从该 tag 另开分支。

## 验收表（P2-AC1–10）

| ID | 标准 | 复现命令 | 当前证据 |
| --- | --- | --- | --- |
| P2-AC1 | 帧 > 2 KiB 可用 | `grep -r "not supported yet" llvm/lib/Target/RVMin` | 无输出；`large-frame.c` 在 QEMU `-O0`/`-O2` 退出码 0 |
| P2-AC2 | 尾调用生效 | `rvmin-run.sh` 中的 `tail` 门禁 | `-O2` 出现 `jalr zero, 0(t0)`（即 `j`）；1e6 层互递归退出码 0，`-O0` 同程序 SIGSEGV |
| P2-AC3 | 跳转表生效 | `grep LJTI`、`grep "jalr zero, 0("` | 8-case switch 在 `-O2` 为一次表 load + 一次 `jalr`；QEMU 退出码 0 |
| P2-AC4 | 常量乘除优化 | `llvm/test/CodeGen/RVMin/mul-const.ll`、`muldiv` 运行 | `+m` 下 `x/7`/`x%10` 无 `__divsi3`/`__modsi3`；`x*3` 为 `add + slli` |
| P2-AC5 | 反汇编闭环 | `check-rvmin-disasm.sh` | 117 条语料 + 6 条标签分支，`.text` 逐字节往返；`llvm-objdump` 无 `<unknown>` |
| P2-AC6 | Zbb 生效 | `llvm/test/CodeGen/RVMin/zbb-zbs.ll`、编码门禁 2e/2f 段 | `+zbb` 下 `clz/cpop/rev8` 各 1 条；19+8 条编码与 `riscv32` 逐字节一致 |
| P2-AC7 | A 扩展可运行 | `atomics.ll`、`run_atomics` | `amoadd.w.aqrl` + `lr.w/sc.w` 循环；QEMU 退出码 0；`seq_cst` 含 `fence` |
| P2-AC8 | 上篇不回归 | `rvmin-verify-all.sh` | 6/6 门禁通过；lit 24/24；128 次 `-verify-machineinstrs` |
| P2-AC9 | 文档完整 | `docs/README.md`、`git tag --list 'rvmin-ch*'` | 24 个章 tag（ch00–ch23）；索引无“（规划中）” |
| P2-AC10 | 改动范围 | `git diff --name-only rvmin-ch14..HEAD` | 54 个文件全部在 `llvm/lib/Target/RVMin/**`、`llvm/test/{CodeGen,MC}/RVMin/**`；+5250/−75 行 |

> P2-AC5 的原计划写的是“97 条语料”。第 22 章把 A 的 20 条并入后，语料变成
> 117 条；门槛只升不降。

## 数字快照（`rvmin-ch22` 之后）

| 项 | 数量 |
| --- | --- |
| 章 tag | 24 个（`rvmin-ch00`…`rvmin-ch23`），全部 annotated |
| lit 测试 | 24 个（CodeGen 13 + MC 11） |
| 编码语料 | 85 RV32I + 12 M + 19 Zbb + 8 Zbs + 20 A = 144 条 |
| 反汇编往返 | 117 条 + 6 条标签分支 |
| e2e | 10 个 C 程序 × 4 个优化级 × 2 个特性集 = 80 个对象，45484 字节 |
| `-verify-machineinstrs` | 128 次 `llc` |
| QEMU 运行 | 9 次（demo×2、large-frame×2、tail、switch、muldiv×2、atomics） |
| 下篇代码量 | 54 文件，+5250/−75 行 |

一键复现：

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop

"$W/llvm/lib/Target/RVMin/scripts/rvmin-verify-all.sh" "$B/bin"
"$W/llvm/lib/Target/RVMin/scripts/rvmin-run.sh"        "$B/bin"
```

预期最后两行：

```text
ALL GATES PASS (run rvmin-run.sh separately for the QEMU check)
PASS: linked ... bytes (rv32i) and ... bytes (rv32i+m), both ran under qemu-riscv32-static
```

## 明确未做（写进合同，而不是藏着）

| 方向 | 现状 | 原因 / 影响 |
| --- | --- | --- |
| C 扩展发射 | 不做 | 需要 16 位 MC 层、对齐/松弛与 `R_RISCV_RVC_*`；会改变全部门禁的字节基线 |
| PIC / GOT / 动态链接 | 不做 | 只支持静态 `ET_EXEC`；`%hi/%lo` 是绝对重定位 |
| 指令调度 / `llvm-mca` | 不做 | 默认 `NoSchedModel`；所有指令按 program order 发射 |
| 特权态 / CSR / 中断 | 不做 | 本机无 `qemu-system-riscv32`；`ecall` 只用于 user-mode 的 write/exit |
| i64 原子 | 回落 libcall | `MaxAtomicSize = 32`；`__atomic_*_8` 需要 libatomic 或自写运行时 |
| `sc.w` 竞争路径 | 只静态验证 | QEMU user mode 单线程，无法在 `lr`/`sc` 之间插入另一个 hart；lit 检查重试分支存在 |
| 浮点 | 软浮点 libcall | 无 F/D 扩展；`float`/`double` 走 `__addsf3` 等 |
| 多 hart / 线程 | 不做 | 运行时没有 `clone` 包装；A 扩展只验证单线程值语义 |
| 大代码模型 | 只支持默认 | `lui/addi` 的 ±2 GiB 范围之外没有 `auipc` 长序列 |

## 练习汇总（15–22 章各一条）

| 章 | 练习 |
| --- | --- |
| 15 | 给 `rvmin-verify-all.sh` 加 `--quick`（跳过 e2e），并测出各门禁耗时占比 |
| 16 | 把 emergency spill slot 的阈值从 2047 改成可配置，找出最小会破的帧尺寸 |
| 17 | 让 `musttail` 在 varargs 调用上也成立，并说明 ABI 为什么允许/不允许 |
| 18 | 给 jump table 增加 `R_RISCV_32_PCREL` 形式，比较表项大小与重定位数量 |
| 19 | 为 `x % 2^n` 增加 `andi` 快路径，并用 `mul-const.ll` 锁定 |
| 20 | 让反汇编器把 `fence` 的 pred/succ 打印成 `iorw` 组合，而不是固定全文 |
| 21 | 把 `rev8` 的 `bswap` 展开与 Zbb 单条做尺寸/周期对照 |
| 22 | 按 ordering 拆 AMO 的 aq/rl 四套 pattern，删掉多余的 fence（见第 22 章练习 1） |

## 第二课方案定稿

第二课不是“再写一个后端”，而是**用自己后端的产物驱动自己写的机器**：

| 项 | 决定 |
| --- | --- |
| 方向 | B：RV32I 模拟器 + 调试器（`rsim`） |
| 分支 | 从 `rvmin-ch23` 开 `course2/rv32-emulator`，不覆盖第一课 |
| 目录 | 仓库根 `course2/`，构建在 `/home/ubuntu/llvm/builds/pr-ctpop/course2` |
| 依赖 | C++17 + CMake，**不链接 LLVM**；`llvm-mc`/`llvm-objdump`/QEMU 只做对拍 |
| 章数 | 12 章（00–11），每章 1 提交 + 八要素 + 门禁 + `rsim-chNN` tag |
| 验收 | C2-AC1–8：ELF 加载、97 条译码、demo 执行、与 QEMU 差分、trace、调试器 transcript、陷阱、范围 |

为什么不是“汇编器 + 链接器”（方向 A）：它和第一课的 MC 层重复度更高，
而且第二课第 6 章的 trace 对照需要反汇编器——第一课 ch20 已经提供了。
完整章节表与风险见 `.omx/plans/2026-11-16-rvmin-course2.md`。

## 常见坑（收尾时踩到的）

1. **“规划中”会漂移。** 章号表、README、`docs/README` 三处要同时改；
   本章用 `grep -n '规划中' docs/README.md README.md` 作为收尾检查（两个索引都必须为空）。
2. **数字要现算，不要抄上一章。** lit 数量、语料条数、`-verify-machineinstrs`
   次数每章都会变；本章的数字全部来自 `rvmin-ch22` 的实测输出。
3. **tag 必须逐章 annotated 且显式 push。** `git push --tags` 在本仓库会卡住；
   每章只推 `feat/riscv-minimal-backend` 与当章 tag。

## 练习

1. 用 `git checkout rvmin-ch16` 到 `rvmin-ch22` 逐个构建，确认每个 tag 都能
   独立通过当章门禁；把失败的 tag 记成 errata。
2. 给 P2-AC1–10 写一个脚本 `check-part2-ac.sh`，把“复现命令”列变成可执行断言。
3. 从“未做清单”里挑一项（建议 i64 原子或 `sc.w` 竞争），写一页设计草案：
   需要改哪些文件、会动哪些门禁、最小可验证实验是什么。
