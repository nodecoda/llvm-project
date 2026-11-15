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
| 09 | 整数类型合法化（i1/i8/i16/i64） | 待做 |
| 10 | RV32I 缺口：乘除、浮点与内存内建 | 待做 |
| 11 | clang 端到端、测试体系与教程收尾 | 待做 |
| 12 | 可选：链接与 QEMU 运行 | 待做 |
| 13 | 可选：M 扩展与优化 | 待做 |

## 当前限制（第 8 章）

- i32 的算术/逻辑/移位/比较、分支、栈对象、load/store、全局数据与字符串、
  函数调用/递归/栈传参全部可用；`-O0`…`-O3` 都能出汇编和对象文件。
- 只合法化 i32 的访存与参数：`char`/`short`、`i64`、结构体按值传递
  （`byval`/`sret`）和 `memcpy` 需要第 9 章的类型合法化。
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
- 乘法/除法/取模与浮点留给第 10 章的 libcall（RV32I 没有 M/F/D 扩展）。
