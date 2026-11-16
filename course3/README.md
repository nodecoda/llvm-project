# 第三课：从零写一台能启动内核的 RV32 机器（rmach）

第一课写了一个 RISC-V 后端，第二课写了一个用户态模拟器。第三课把“机器”补齐：
**特权级、CSR、精确定时器中断、Sv32 页表、UART**，并让一个最小 M 态内核
真正进入 U 态、跑用户程序、被定时器抢占。

```text
   内核（clang, M 态）              用户程序（rvmin, U 态）
        │                                   │
        ├── 建 Sv32 页表 ──> satp            │
        ├── mret ─────────> 进入 U ──────────┘
        ├── ecall/write/exit <── 系统调用
        └── CLINT 定时器中断 <── 抢占
                        │
                        v
              rmach：取指 → 翻译 → 执行 → 中断
```

`rmach` 不复用第二课的 `Cpu`：取指与访存路径要经过特权级和 MMU，继承只会
让两边都难改。它只读复用第二课已经验证过的 `decode`、`elf`、`memory`。

## 前置

- 第一课收口于 `rvmin-ch23`，第二课收口于 `rsim-ch11`；
- LLVM 24 的 `clang`、`llvm-mc`、`llvm-readelf`、`llvm-objdump`；
- `ld.lld`（系统 `/usr/bin/ld.lld` 即可）；
- 可选：`qemu-system-riscv32`（仅第 05 章设备冒烟，不是主门禁）。

## 构建

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop

cmake -S "$W/course3" -B "$B/course3" -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C "$B/course3"
"$B/course3/rmach" --version
"$W/course3/scripts/course3-verify.sh" "$B/bin" "$B/course3/rmach"
```

第 00 章的输出：

```text
rmach 0.0 (chapter 00)
0 checks
```

## 章节

| 章 | 主题 | tag |
| --- | --- | --- |
| 00 | 导览与骨架 | `c3-ch00` |
| 01 | 裸机镜像与启动 | `c3-ch01` |
| 02 | CSR 文件与特权级 | `c3-ch02` |
| 03 | 精确陷阱与 `mret` | `c3-ch03` |
| 04 | CLINT 与定时器中断 | `c3-ch04` |
| 05 | MMIO 总线与 UART | `c3-ch05` |
| 06 | Sv32 分页 | `c3-ch06` |
| 07 | U 态与系统调用 | `c3-ch07` |
| 08 | 上下文与两进程调度 | `c3-ch08` |
| 09 | 调试与可观测性 | `c3-ch09` |
| 10 | 第三课验收 | `c3-ch10` |

每章的契约与第一、第二课一致：1 个提交 + 八要素文档 + 可判定门禁 + annotated tag。

## 边界

- 只改 `course3/**`；`course2/**` 只读复用，`llvm/lib/Target/RVMin/**` 冻结；
- 只做 M + U 两级特权，不做 S 态、PLIC 完整模型、多 hart、浮点、DMA；
- 内核用 clang 的标准 RISC-V 后端编译，用户程序继续用第一课的 rvmin；
- 设备地址对齐 QEMU `virt`，但系统态 QEMU 只作可选冒烟。
