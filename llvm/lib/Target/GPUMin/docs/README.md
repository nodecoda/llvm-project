# GPUMin 教程：从零实现一个 SIMT 教学后端

GPUMin 是一个最小 SIMT（GPU）LLVM 后端。它的目标不是跑得快，而是**每一层
都能被读到、被验证**：从一条 32 位指令的位域，到一个 block 的 barrier
释放条件。

## 它是什么 / 不是什么

**是**：

- 一个完整的 LLVM target：`llc -march=gpumin`、`llvm-mc -triple=gpumin`；
- 一个可执行的 SIMT 模型：warp/lane、active mask、结构化分歧、共享内存、
  barrier、发射槽、延迟；
- 一个逐章可验证的教程：每章一个门禁脚本，8 项总门禁。

**不是**：

- 不是完整 GPU：没有纹理单元、没有 warp 级原语（`shfl`）、没有
  coalescing 模型、没有多 block 驻留调度；
- 不是 cycle-accurate：第 18/19 章是**教学**时序模型（每单元固定发射槽 +
  固定延迟），不是某块真实芯片的流水线；
- 不是性能工具：`--stats` 里的数字用于**比较两个实现的形状**，不能外推
  到真实硬件的绝对性能。

## 快速开始

```sh
# 构建（在 LLVM 构建目录）
ninja llc llvm-mc llvm-objdump

# 编译一个 kernel
llc -march=gpumin -O1 -filetype=obj scripts/e2e/vecadd.ll -o /tmp/vecadd.o

# 在模拟器上跑
python3 tools/gpumin_sim.py /tmp/vecadd.o --kernel vecadd \
  --blocks 2 --threads 8 --arg 0x0 --arg 0x2000 --arg 0x4000 --arg 8 \
  --fill 0x0=0,1,8 --fill 0x2000=0,3,8 --dump 0x4000=8 --stats

# 全部门禁
scripts/gpumin-verify-all.sh <llvm-bin-dir>
```

## 章节索引

| # | 章节 | 门禁 | 主题 |
| --- | --- | --- | --- |
| 00 | [overview](00-overview.md) | `check-ch00.sh` | target 骨架、triple、注册 |
| 01 | [registers-formats](01-registers-formats.md) | `check-ch01.sh` | 寄存器、指令格式、opcode 表 |
| 02 | [mc-layer](02-mc-layer.md) | `check-ch02.sh` | MC 编码与独立编码器对拍 |
| 03 | [minimal-function](03-minimal-function.md) | `check-ch03.sh` | 最小函数、`ret`/`exit` |
| 04 | [alu-and-constants](04-alu-and-constants.md) | `check-ch04.sh` | ALU、立即数、32 位常量 |
| 05 | [control-flow](05-control-flow.md) | `check-ch05.sh` | 分支、谓词、基本块 |
| 06 | [global-memory](06-global-memory.md) | `check-ch06.sh` | `addrspace(1)`、符号与重定位 |
| 07 | [calls-and-stack](07-calls-and-stack.md) | `check-ch07.sh` | 调用约定、栈帧、`lr` |
| 08 | [types-and-legalization](08-types-and-legalization.md) | `check-ch08.sh` | i1/i8/i16 合法化 |
| 09 | [special-registers](09-special-registers.md) | `check-ch09.sh` | `sr.*`、funct 表 |
| 10 | [static-acceptance](10-static-acceptance.md) | `gpumin-verify-all.sh` | 静态面锁定、8 项门禁 |
| 11 | [simt-model](11-simt-model.md) | `check-ch11.sh` | 冻结 SIMT 参数（JSON 为源） |
| 12 | [simulator](12-simulator.md) | `check-ch12.sh` | ELF 解码、执行、内存 |
| 13 | [kernel-launch](13-kernel-launch.md) | `check-ch13.sh` | kernel ABI、launch descriptor |
| 14 | [divergence](14-divergence.md) | `check-ch14.sh` | 结构化分歧、帧栈、重汇聚 |
| 15 | [simt-acceptance](15-simt-acceptance.md) | `check-ch15.sh` | 三个 kernel、`--stats` |
| 16 | [shared-memory](16-shared-memory.md) | `check-ch16.sh` | `addrspace(2)`、静态偏移、`sr.shared` |
| 17 | [barrier](17-barrier.md) | `check-ch17.sh` | `bar.sync`、block 调度、竞态 |
| 18 | [scheduling-model](18-scheduling-model.md) | `check-ch18.sh` | ALU/LSU/BAR 发射槽、`cycles` |
| 19 | [latency-occupancy](19-latency-occupancy.md) | `check-ch19.sh` | 访存延迟、occupancy |
| 20 | [matmul-acceptance](20-matmul-acceptance.md) | `check-ch20.sh` | tiling 的代价与收益 |
| 21 | [triton-frontend](21-triton-frontend.md) | `check-ch21.sh` | C++ 前端、CPython AST 桥 |
| 22 | [ttir-min](22-ttir-min.md) | `check-ch22.sh` | TTIR-min、SSA、标量 lowering |
| 23 | [tensor-semantics](23-tensor-semantics.md) | `check-ch23.sh` | tensor、指针、mask、`tt.addptr` |
| 24 | [fixed-layout-llvm](24-fixed-layout-llvm.md) | `check-ch24.sh` | 固定布局、标量化、`sr.args` ABI |
| 25 | [f32](25-f32.md) | `check-ch25.sh` | f32 ISA、`addf/mulf/cmpf`、浮点常量 |
| 26 | [reduction](26-reduction.md) | `check-ch26.sh` | `tt.reduce`、shared、双 barrier |
| 27 | [triton-e2e](27-triton-e2e.md) | `check-ch27.sh` + `triton-verify-all.sh` | `tl.where`、编译期循环、四个 e2e |
| 28 | [elementwise](28-elementwise.md) | `check-ch28.sh` + `triton-verify-all.sh` | `//`/`%`、`tl.cdiv`、min/max、f32 `tl.max` |
| 29 | [layout2d](29-layout2d.md) | `check-ch29.sh` + `triton-verify-all.sh` | 2D tensor、`expand_dims`/`broadcast`/`trans`、2D grid、`mul` |
| 30 | [matmul2d](30-matmul2d.md) | `check-ch30.sh` + `triton-verify-all.sh` | rank-1 update matmul、4×4 grid、2D zeros、f32 fill |
| 31 | [softmax2d](31-softmax2d.md) | `check-ch31.sh` + `triton-verify-all.sh` | 2D 归约、`keep_dims`、`exp2`/`fdiv`、逐行 softmax |
| 32 | [attention](32-attention.md) | `check-ch32.sh` + `triton-verify-all.sh` | QK^T → softmax → PV、`tl.debug_barrier()`、第三课收束 |
| 33 | [runtime-if](33-runtime-if.md) | `check-ch33.sh` + `triton-verify-all.sh` | `scf.if` region、then/else/merge、分支作用域、第四课开篇 |
| 34 | [shared-tile](34-shared-tile.md) | `check-ch34.sh` + `triton-verify-all.sh` | shared staging、列/行切片、自动 barrier、`glb_ld` 5× |
| 35 | [tl-dot](35-tl-dot.md) | `check-ch35.sh` + `triton-verify-all.sh` | `tt.dot`、`inputPrecision = tf32`、无 MMA 展开 |
| 36 | [causal-mask](36-causal-mask.md) | `check-ch36.sh` + `triton-verify-all.sh` | mask 广播、`other=`、加性 causal mask |
| 37 | [course4-wrap](37-course4-wrap.md) | `check-ch37.sh` + `bench-gpumin.sh` | 七内核基准表、上限门禁、第四课收束 |

## 8 项总门禁

`scripts/gpumin-verify-all.sh` 覆盖：

1. target 注册与 `llvm-mc` 可用；
2. MC 编码与 `tools/gpumin-encode.py` 逐字节一致；
3. MC lit：汇编/反汇编/重编码；
4. CodeGen lit；
5. 静态 e2e：IR → `.o`，且 `.o` 与重汇编的 `.s` 一致；
6. 模拟器与 launch e2e（第 12–20 章的门禁）；
7. 调度与 occupancy 模型；
8. 文档、`simt-model.json` 与静态计数一致。

## 单一事实来源

`scripts/simt-model.json` 是 warp 大小、special register funct、地址空间、
barrier、调度参数、occupancy 的唯一来源。文档、模拟器、门禁都从它读；
`tools/gpumin_model.py --selftest` 校验它自洽。
