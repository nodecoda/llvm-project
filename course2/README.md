# 第二课：从零写一个 RV32I 模拟器（rsim）

第一课（`feat/riscv-minimal-backend`）写了一个能编译、能反汇编、能在 QEMU 上跑的
RISC-V 后端。第二课把方向反过来：**自己写一台机器，去执行第一课的产物**。

```text
   C 源码 ──clang──> LLVM IR ──llc -march=rvmin──> RV32I 汇编
                                                        │
                                              llvm-mc / ld.lld
                                                        │
                                                        v
                                                 静态 ELF (ET_EXEC)
                                                        │
                                                        v
                                        rsim：加载 → 译码 → 执行 → 调试
```

`rsim` 不链接 LLVM，也不调用 QEMU；它只把 `llvm-mc`、`llvm-objdump` 和
`qemu-riscv32-static` 当作对拍工具（裁判），自己实现 ELF 加载、指令语义、
系统调用、跟踪与调试器。

## 前置

- 第一课收口到 `rvmin-ch23`（后端、反汇编器、A/Zbb 都已就绪）；
- C++17 编译器、CMake ≥ 3.16、Ninja；
- `/usr/bin/qemu-riscv32-static` 与 `/usr/bin/ld.lld`（差分测试用）。

## 构建（构建目录在工作树外）

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop

cmake -S "$W/course2" -B "$B/course2" -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C "$B/course2"
"$B/course2/rsim" --version
"$W/course2/scripts/course2-verify.sh" "$B/course2/rsim"
```

第 00 章的门禁输出就是：

```text
rsim 0.0 (chapter 00)
0 checks
```

## 章节

| 章 | 主题 | tag |
| --- | --- | --- |
| 00 | 导览与骨架 | `rsim-ch00` |
| 01 | ELF32 加载器 | `rsim-ch01` |
| 02 | 内存、栈与启动 | `rsim-ch02` |
| 03 | 译码与执行 RV32I | `rsim-ch03` |
| 04 | 控制流与精确异常 | `rsim-ch04` |
| 05 | 系统调用与运行时 | `rsim-ch05` |
| 06 | 跟踪与自反汇编 | `rsim-ch06` |
| 07 | 断点、单步与脚本调试 | `rsim-ch07` |
| 08 | 陷阱模型与 ebreak | `rsim-ch08` |
| 09 | M/Zbb/A 可选指令 | `rsim-ch09` |
| 10 | 指令统计与“性能” | `rsim-ch10` |
| 11 | 第二课验收 | `rsim-ch11` |

每章的契约与第一课一致：1 个提交 + 八要素文档 + 可判定门禁 + annotated tag。

## 边界

- 只改 `course2/**`；第一课目录冻结（除 errata 外不动）；
- 用户态、单线程、静态 ELF；不做 MMU、特权态、设备模型、动态链接；
- 调试器先做自带 CLI，GDB RSP 只作为附录（本机 gdb 不支持 `riscv:rv32`）。
