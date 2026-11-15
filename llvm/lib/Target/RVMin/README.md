# RVMin: a from-scratch RV32I backend

RVMin 是一个教学用的 LLVM 后端：不写前端、不复用 `llvm/lib/Target/RISCV` 的代码，
只实现 RISC-V 最小整数指令集（RV32I）。前端使用 clang 产出 LLVM IR，
后端通过 `llc -march=rvmin` 选择。

## 快速开始

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
| 11 | clang 端到端、测试体系与教程收尾 | 待做 |
| 12 | 可选：链接与 QEMU 运行 | 待做 |
| 13 | 可选：M 扩展与优化 | 待做 |

## 当前限制（第 10 章）

- i32 的算术/逻辑/移位/比较、分支、栈对象、load/store、全局数据与字符串、
  函数调用/递归/栈传参全部可用；`-O0`…`-O3` 都能出汇编和对象文件。
- 窄类型可用：`char`/`short` 的读写降到 `LB`/`LBU`/`LH`/`LHU`/`SB`/`SH`，
  `sign_extend_inreg` 展开成 `slli`+`srai`，返回值按 ABI 扩展。
- `i64` 被拆成两个 `i32`：加减、比较、load/store、常数移位都可用；变长移位
  展开成 `__ashldi3`/`__lshrdi3`/`__ashrdi3` 调用（需要运行时库）。
  结构体按值拷贝经 `memcpy` 库调用工作（复用第 8 章的调用支持）。
- 乘除取模可用：`i32`/`i64` 的 `mul`/`div`/`rem`（有符号与无符号）走
  `__mulsi3`/`__divsi3`/`__udivsi3`/`__modsi3`/`__umodsi3`/`__muldi3`/`__divdi3`
  等 libcall；2 的幂的乘法与常量除法仍内联成移位。
- 位运算内建内联展开：`cpop`/`clz`/`ctz` 用算术序列，`bswap32` 用移位对，
  不产生调用。
- 软浮点可用：`float`/`double` 的运算、比较与整数互转全部变成
  `__addsf3`/`__muldf3`/`__ltsf2`/`__fixsfsi`/`__floatsisf` 等 libcall
  （前提是链接时能提供 `libgcc`/`compiler-rt`）。
- `byval`/`sret`、变参函数仍然显式报错。
- 帧内偏移限制在 `[-2048, 2047]`；更大的帧会明确报错（后续用
  `RegScavenger` 物化地址）。没有帧指针、没有 VLA/动态 `alloca`。
- 直接调用固定展开为 `LUI+ADDI+JALR`，使用 `R_RISCV_HI20`/`LO12_I`；
  没有 `R_RISCV_CALL`/`R_RISCV_RELAX`，因此也没有链接器松弛。间接调用是
  单条 `jalr ra, rs1, 0`。
- 没有尾调用；变参函数、`byval`/`sret` 仍显式报错。
- `.eh_frame` 只对无栈帧函数正确：还没发 CFI 栈调整指令（第 11 章）。
- MC 层完整：`llvm-mc -triple=rvmin` 可汇编 83 条指令（调用序列复用
  `lui/addi/jalr`），立即数、标签和 `%hi/%lo` 三类编码都与参考 RISC-V
  汇编器逐字节一致（`scripts/check-rvmin-encodings.sh`）。
- 分支/跳转范围是 B 型 ±4 KiB、J 型 ±1 MiB，没有 branch relaxation。
- 非 2 的幂的常量乘法仍然走 `__mulsi3`（正确但慢，第 13 章可加 DAG combine）。
- 后端只发 libcall，不提供实现：链接需要 `libgcc`/`compiler-rt`（第 12 章可选做
  一个最小运行时）。
- 窄类型的参数依赖调用方按 ABI 扩展；函数入口不重新扩展，手写 IR 传入脏高位
  会得到错误结果。
