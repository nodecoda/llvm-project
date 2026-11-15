# 第 15 章：下篇导览与基线核对

## 目标

上篇（第 0–14 章）从零写出一个能编译、能链接、能在 QEMU 里运行的 RV32I(+M) 后端，
并以验收章收口。下篇不再增加“能不能用”的能力，而是回答三个新问题：

1. **能不能更稳**：帧大于 2 KiB、尾调用、跳转表；
2. **能不能更快**：常量乘除的强度削减；
3. **能不能看得见**：反汇编器、位操作扩展、原子指令。

本章不写新功能，只做三件事：给出上篇能力清单、下篇路线图，以及一个能一次跑完
所有门禁的脚本 `scripts/rvmin-verify-all.sh`。读完本章你应该能判断：当前工作区
是否与上篇结束时一致，以及接下来每章会改哪些文件。

> 每章都可单独 checkout：`git checkout rvmin-ch16` 就是第 16 章结束时的完整代码。
> 上篇对应 `rvmin-ch00` … `rvmin-ch14`。

## 背景：为什么分上下篇

上篇的叙事是“从零到能跑”，每章都在补一个**缺失**的机制；下篇的叙事是
“从能跑到像样”，每章都在**改写**上篇已有的决定：

| 上篇的决定 | 位置 | 下篇的改写 |
| --- | --- | --- |
| 帧偏移超出 12 位直接报错 | `RVMinRegisterInfo.cpp:72-74` | 第 16 章用 `RegScavenger` 物化地址 |
| `CLI.IsTailCall = false` | `RVMinISelLowering.cpp:350` | 第 17 章实现尾调用 |
| `BR_JT`/`BRIND` 设为 `Expand` | `RVMinISelLowering.cpp:130-134` | 第 18 章实现跳转表 |
| 常量乘除一律 libcall | `RVMinISelLowering.cpp:100-108` | 第 19 章魔数乘法 |
| 只有 AsmParser | `AsmParser/` | 第 20 章加反汇编器 |
| 位内建一律算术展开 | `RVMinISelLowering.cpp:114-119` | 第 21 章 Zbb/Zbs |
| 无原子指令 | — | 第 22 章 A 扩展 |

因此下篇是**同一门课的第二部分**，不是新课程：同一份代码、同一条分支、同一套
“每章一个提交 + 门禁 + 八要素文档”的契约。

## 上篇能力清单

| 能力 | 状态 | 证据 |
| --- | --- | --- |
| target 注册、`llc -march=rvmin` | ✅ | `llc --version` |
| RV32I 39 条指令 + M 8 条 | ✅ | `scripts/rvmin-encoding-corpus.s`、`scripts/rvmin-m-encoding-corpus.s` |
| 逐字节编码一致性 | ✅ | `scripts/check-rvmin-encodings.sh`（85 + 12 条） |
| 整数 ALU、比较、select、大常量 | ✅ | `llvm/test/CodeGen/RVMin/alu.ll` |
| 分支、基本块布局 | ✅ | `control.ll` |
| 栈帧、`alloca`、load/store | ✅ | `stack.ll` |
| 全局数据、`%hi/%lo` 重定位 | ✅ | `globals.ll` + `llvm-readelf -r` |
| 调用、递归、callee-saved | ✅ | `calls.ll` |
| i1/i8/i16/i64 合法化 | ✅ | `types.ll` |
| libcall、位内建、软浮点 | ✅ | `libcalls.ll` |
| `.eh_frame`、`switch` 比较链 | ✅ | `cfi.ll` |
| 链接 + QEMU 运行 | ✅ | `scripts/rvmin-run.sh` |
| 可选 M 扩展 | ✅ | `m-ext.ll`、`-mattr=+m` |
| 反汇编器 / 大帧 / 尾调用 / 跳转表 | ❌ | 下篇 |

## 下篇路线

| 章 | 主题 | 消除的限制 | 主要文件 |
| --- | --- | --- | --- |
| 15 | 导览与基线 | — | 本章 + `rvmin-verify-all.sh` |
| 16 | 大栈帧与 RegScavenger | 帧 > 2 KiB 报错 | `RVMinRegisterInfo.cpp`、`RVMinFrameLowering.cpp` |
| 17 | 尾调用 | `return f(x)` 不能复用栈帧 | `RVMinISelLowering.cpp`、`RVMinInstrInfo.td` |
| 18 | 跳转表与间接跳转 | `switch` 只有比较链 | `RVMinISelLowering.cpp`、`RVMinInstrInfo.td` |
| 19 | 常量乘除强度削减 | `x/7`、`x*3` 仍是 libcall/乘法 | `RVMinISelLowering.cpp` |
| 20 | 反汇编器 | 只有 AsmParser | `Disassembler/`、`CMakeLists.txt` |
| 21 | Zbb/Zbs | `clz/cpop/rev8` 是算术序列 | `RVMin.td`、`RVMinInstrInfo.td` |
| 22 | A 扩展 | 无 `lr/sc`、无原子 | `RVMin.td`、`RVMinInstrInfo.td`、`RVMinISelLowering.cpp` |
| 23 | 下篇验收 | — | AC 表 + 未做清单 |

## 基线核对：一次跑完所有门禁

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop
"$W/llvm/lib/Target/RVMin/scripts/rvmin-verify-all.sh" "$B/bin"
```

预期输出（顺序固定）：

```console
PASS 1/5: target registered
Total Discovered Tests: 14
  Passed: 14 (100.00%)
PASS 2/5: lit (CodeGen + MC)
PASS: 85 RV32I instructions + 12 M-extension instructions, .text identical
PASS 3/5: encodings
PASS: 10 programs x 0 1 2 3 optimization levels x 2 feature sets
PASS 4/5: e2e
PASS 5/5: 80 llc runs with -verify-machineinstrs
ALL GATES PASS (run rvmin-run.sh separately for the QEMU check)
```

QEMU 那一项单独跑，因为它需要链接器和模拟器：

```bash
"$W/llvm/lib/Target/RVMin/scripts/rvmin-run.sh" "$B/bin"
# rv32i: rvmin: ok
# rv32i+m: rvmin: ok
# PASS: linked 6656 bytes (rv32i) and 6592 bytes (rv32i+m), ...
```

## 速查：下篇会反复用到的调试手段

| 目的 | 命令 | 看什么 |
| --- | --- | --- |
| 机器指令选择结果 | `llc -stop-after=finalize-isel -o -` | `MIR` 里的伪指令与 `FI` 操作数 |
| 帧消除之后 | `llc -print-after=prolog-epilog -o -` | `sp` 调整、`lw/sw` 的偏移 |
| 只跑 PEI | `llc -run-pass=prologepilog -o -` | 帧索引是否被替换 |
| 合法化过程 | `llc -debug-only=legalize-types`（assert 构建） | 哪个节点被 `Expand`/`Promote` |
| 选择失败定位 | `llc -debug-only=isel` | `Cannot select` 的节点形状 |
| 反汇编（第 20 章前） | `llvm-objdump -d x.o` | 目前借用参考 RISC-V 反汇编器 |
| 重定位 | `llvm-readelf -r x.o` | `R_RISCV_HI20/LO12_*`、`R_RISCV_32` |
| CFI | `llvm-readelf --unwind x.o` | CFA 与 `ra` 的偏移 |

## 常见坑（下篇特有）

- **改 `FrameLowering` 会同时影响 `-O0` 和 `-O2`**：`-O0` 用 fast regalloc，溢出更多，
  大帧路径最容易在这里暴露。每章至少跑一遍 `-O0..-O3`。
- **`-verify-machineinstrs` 是必需品，不是可选项**：`RegScavenger`、尾调用、跳转表
  都会手工改 `MachineInstr`，寄存器/操作数错误只有它抓得住。
- **汇编与对象两条路径都要看**：`-o -` 走 AsmPrinter，`-filetype=obj` 走 MC 编码器，
  第 18 章的跳转表和第 20 章的反汇编器只在其中一条路径上出问题。
- **`+m` 与默认构建要同时过**：第 19 章的优化只在有 M 时启用，默认构建必须继续走 libcall。
- **不要改上篇章节的文档**：上篇限制在下篇章节里“解除”，这样按 tag checkout 的读者
  看到的仍是当时的真实状态。

## 练习

1. 用 `rvmin-verify-all.sh` 在 `rvmin-ch14` 上跑一遍，确认输出与本章一致。
2. 把脚本第 5 项改成只扫 `-O2`，比较耗时；再想一想为什么上篇坚持扫四个优化级。
3. 用 `llc -stop-after=finalize-isel` 看一个带 `alloca` 的函数，找到 `FI` 操作数，
   再对照 `-print-after=prolog-epilog` 看它变成了什么——这就是第 16 章要改的位置。

## 下一章

第 16 章从 `RVMinRegisterInfo.cpp:72-74` 的 `report_fatal_error` 开始：先写一个
3 KiB 帧的测试把它复现出来，再用 `RegScavenger` 把地址物化到临时寄存器。
