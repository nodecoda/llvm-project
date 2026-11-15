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
| 04 | 整数算术、逻辑、移位、比较与 select | 待做 |
| 05 | 控制流与基本块布局 | 待做 |
| 06 | 栈、局部变量与 load/store | 待做 |
| 07 | 全局数据、字符串与重定位 | 待做 |
| 08 | 调用约定、函数调用与递归 | 待做 |
| 09 | 整数类型合法化（i1/i8/i16/i64） | 待做 |
| 10 | RV32I 缺口：乘除、浮点与内存内建 | 待做 |
| 11 | clang 端到端、测试体系与教程收尾 | 待做 |
| 12 | 可选：链接与 QEMU 运行 | 待做 |
| 13 | 可选：M 扩展与优化 | 待做 |

## 当前限制（第 3 章）

- 已有最小指令选择：常量、`add`、`ret` 能出 RV32I 汇编，
  `llvm/test/CodeGen/RVMin/basic.ll` 全绿；clang IR 端到端可用。
- 调用约定只覆盖寄存器传参（a0–a7）与寄存器返回（a0/a1）。
- 还没有函数调用、栈帧、控制流、大部分 ALU、全局与重定位。
- MC 层仍然完整：`llvm-mc -triple=rvmin` 可汇编 67 条指令，编码与参考
  RISC-V 汇编器逐字节一致（`scripts/check-rvmin-encodings.sh`）。
- 对象文件里的符号重定位（含 `.eh_frame` 的函数符号引用）第 7 章实现；
  当前遇到未定义符号会明确报错。
