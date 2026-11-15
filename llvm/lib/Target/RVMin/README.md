# RVMin: a from-scratch RV32I backend

RVMin 是一个教学用的 LLVM 后端：不写前端、不复用 `llvm/lib/Target/RISCV` 的代码，
只实现 RISC-V 最小整数指令集（RV32I）。前端使用 clang 产出 LLVM IR，
后端通过 `llc -march=rvmin` 选择。

## 快速开始

完整章节索引与验收对照见 [`docs/README.md`](docs/README.md)。

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop

# 1) 在现有构建目录里启用实验 target（不要新建 build dir）
cmake -S "$W/llvm" -B "$B" -DLLVM_EXPERIMENTAL_TARGETS_TO_BUILD=RVMin
ninja -C "$B" llc clang llvm-mc llvm-objdump llvm-readelf FileCheck

# 2) 确认 target 已注册
"$B/bin/llc" --version | grep -w rvmin

# 3) clang 前端 → LLVM IR
"$B/bin/clang" --target=riscv32-unknown-elf -march=rv32i -mabi=ilp32 \
  -O1 -S -emit-llvm example.c -o example.ll

# 4) RVMin 后端
"$B/bin/llc" -march=rvmin -mtriple=riscv32-unknown-elf example.ll -o example.s
```

为什么是 `llc -march=rvmin` 而不是 `clang --target=rvmin`：

- RVMin 没有 `Triple::ArchType` 映射，也没有改 clang 的 target 表。
- `TargetRegistry::lookupTarget` 支持“按名字选择、没有 triple 映射的后端”
  （见 `llvm/lib/MC/TargetRegistry.cpp`）；clang 给出的 `riscv32` triple
  会原样保留，因此数据布局和 ILP32 ABI 与 clang 一致。
- 这样整个项目只新增 `llvm/lib/Target/RVMin/`，不改核心与 clang。

## 章节

教程分上下两篇。上篇从零写出一个可运行的后端，下篇在这个后端上继续加深。
每章结束时的代码都可以按 tag checkout：

```bash
git checkout rvmin-ch14     # 上篇结束
git checkout rvmin-ch16     # 第 16 章（大栈帧）结束
```

### 上篇：从零到能跑（00–14，已完成）

| 章 | 主题 | 状态 |
| --- | --- | --- |
| 00 | [target 骨架与注册](docs/00-overview.md) | 完成 |
| 01 | [寄存器与 RV32I 指令描述](docs/01-instructions.md) | 完成 |
| 02 | [MC 层、汇编输出与编码正确性](docs/02-mc-layer.md) | 完成 |
| 03 | [最小指令选择与栈帧骨架](docs/03-isel.md) | 完成 |
| 04 | [整数算术、逻辑、移位、比较与 select](docs/04-alu.md) | 完成 |
| 05 | [控制流、条件分支与基本块布局](docs/05-control.md) | 完成 |
| 06 | [栈帧、alloca 与 load/store](docs/06-stack.md) | 完成 |
| 07 | [全局数据、字符串与重定位](docs/07-globals.md) | 完成 |
| 08 | [调用约定、函数调用与递归](docs/08-calls.md) | 完成 |
| 09 | [整数类型合法化（i1/i8/i16/i64）](docs/09-types.md) | 完成 |
| 10 | [RV32I 缺口：乘除、位内建与软浮点](docs/10-libcalls.md) | 完成 |
| 11 | [clang 端到端、测试体系与教程收尾](docs/11-e2e.md) | 完成 |
| 12 | [链接与 QEMU 运行（运行时、ecall）](docs/12-run.md) | 完成 |
| 13 | [可选 M 扩展与优化空间](docs/13-m-extension.md) | 完成 |
| 14 | [验收：AC1–AC10 逐条证据](docs/README.md) | 完成 |

### 下篇：从能跑到像样（15–23，进行中）

| 章 | 主题 | 状态 |
| --- | --- | --- |
| 15 | [下篇导览与基线核对](docs/15-part2-overview.md) | 完成 |
| 16 | 大栈帧与 RegScavenger | 规划中 |
| 17 | 尾调用与 musttail | 规划中 |
| 18 | 跳转表与间接跳转 | 规划中 |
| 19 | 常量乘除的强度削减 | 规划中 |
| 20 | 反汇编器 | 规划中 |
| 21 | Zbb/Zbs 位操作扩展 | 规划中 |
| 22 | A 扩展与 AtomicExpand | 规划中 |
| 23 | 下篇验收 | 规划中 |

## 当前限制（上篇结束时，第 14 章）

- i32 的算术/逻辑/移位/比较、分支、栈对象、load/store、全局数据与字符串、
  函数调用/递归/栈传参全部可用；`-O0`…`-O3` 都能出汇编和对象文件。
- 窄类型可用：`char`/`short` 的读写降到 `LB`/`LBU`/`LH`/`LHU`/`SB`/`SH`，
  `sign_extend_inreg` 展开成 `slli`+`srai`，返回值按 ABI 扩展。
- `i64` 被拆成两个 `i32`：加减、比较、load/store、常数移位都可用；变长移位
  展开成 `__ashldi3`/`__lshrdi3`/`__ashrdi3` 调用（需要运行时库）。
  结构体按值拷贝经 `memcpy` 库调用工作（复用第 8 章的调用支持）。
- 乘除取模可用，且 M 扩展是**可选**的：默认（纯 RV32I）走
  `__mulsi3`/`__divsi3`/`__udivsi3`/`__modsi3`/`__umodsi3` 等 libcall；
  加 `-mattr=+m`（或 `-mcpu=generic-rv32-m`）后变成单条
  `mul/mulh/mulhu/div/divu/rem/remu`。2 的幂的乘法与常量除法仍内联成移位。
  汇编器在没打开 M 时会报 `instruction requires the following: 'M' ...`。
- 位运算内建内联展开：`cpop`/`clz`/`ctz` 用算术序列，`bswap32` 用移位对，
  不产生调用。
- 软浮点可用：`float`/`double` 的运算、比较与整数互转全部变成
  `__addsf3`/`__muldf3`/`__ltsf2`/`__fixsfsi`/`__floatsisf` 等 libcall
  （前提是链接时能提供 `libgcc`/`compiler-rt`）。
- `byval`/`sret`、变参函数仍然显式报错。
- `switch` 降级为比较链（`BR_JT`/`BRIND` 不合法）：没有跳转表，也没有间接跳转。
- `.eh_frame` 为需要 unwind 信息的函数生成（手写 IR、`-funwind-tables`）；
  包含 CFA、`ra` 与 callee-saved 的偏移。
- 帧内偏移限制在 `[-2048, 2047]`；更大的帧会明确报错（后续用
  `RegScavenger` 物化地址）。没有帧指针、没有 VLA/动态 `alloca`。
- 直接调用固定展开为 `LUI+ADDI+JALR`，使用 `R_RISCV_HI20`/`LO12_I`；
  没有 `R_RISCV_CALL`/`R_RISCV_RELAX`，因此也没有链接器松弛。间接调用是
  单条 `jalr ra, rs1, 0`。
- 没有尾调用（`return f(x);` 先调用再返回）；`x * 3` 这类常量乘法还没有
  strength reduction。
- MC 层完整：`llvm-mc -triple=rvmin` 可汇编 85 条 RV32I 指令（调用序列复用
  `lui/addi/jalr`）外加 8 条 M 指令（需 `-mattr=+m`），立即数、标签和
  `%hi/%lo` 三类编码都与参考 RISC-V 汇编器逐字节一致
  （`scripts/check-rvmin-encodings.sh`）。
- 分支/跳转范围是 B 型 ±4 KiB、J 型 ±1 MiB，没有 branch relaxation。
- 非 2 的幂的常量乘法默认走 `__mulsi3`、`+m` 时走 `mul`；还没有
  `x * 3 → slli + add` 这类 strength reduction。
- 默认（纯 RV32I）后端只发 libcall；`scripts/rt/rvmin-rt.S` 提供 8 个整数
  实现（乘除取模、64 位移位），其余（`__muldi3`、软浮点等）仍需
  `libgcc`/`compiler-rt`。打开 M 后乘除取模不再需要运行时。
- 可运行：`scripts/rvmin-run.sh` 用 clang → llc → **自己的汇编器** → `ld.lld`
  → `qemu-riscv32-static` 跑通一个自检程序（含 8 个 libcall 的最小运行时
  `scripts/rt/rvmin-rt.S`、`_start` 与链接脚本），并把同一程序在 RV32I 与
  RV32I+M 两种构建下各跑一次，要求输出一致、`+m` 版本不再引用乘除 libcall。
- 测试体系：14 个 lit 测试（10 个 CodeGen + 4 个 MC；每个 CodeGen 测试都有一条
  `llvm-mc -triple=riscv32` 往返 RUN 行）+ 93 条指令的编码门禁 +
  `scripts/rvmin-e2e.sh`（10 个 C 程序 × `-O0..-O3` × 2 个特性集 = 80 个对象，
  `.text` 与 `llvm-mc` 重汇编逐字节一致）。
- 窄类型的参数依赖调用方按 ABI 扩展；函数入口不重新扩展，手写 IR 传入脏高位
  会得到错误结果。
