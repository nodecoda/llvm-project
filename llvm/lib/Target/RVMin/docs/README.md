# RVMin 教程索引

RVMin 是一个从零写的 LLVM 后端，只实现 RISC-V 最小整数集（RV32I，可选 M 扩展）。
本目录是配套教程：每章一个提交，每章都能独立构建、测试、复现。

- 总览与快速开始：[`../README.md`](../README.md)
- 完整实现：`llvm/lib/Target/RVMin/`
- 门禁脚本：`scripts/`（编码、端到端、链接运行）

## 章节

### 上篇：从零到能跑（00–14）

| 章 | 文档 | 主题 | 本章门禁 | 提交 / tag |
| --- | --- | --- | --- | --- |
| 00 | [00-overview.md](00-overview.md) | target 骨架与注册 | `llc --version \| grep rvmin` | `d65f87020db7` |
| 01 | [01-instructions.md](01-instructions.md) | 寄存器与 RV32I 六种指令格式 | `llvm-mc -triple=rvmin -show-encoding` | `9c6537834376` |
| 02 | [02-mc-layer.md](02-mc-layer.md) | MC 层、汇编器、逐字节编码 | `scripts/check-rvmin-encodings.sh` | `99a0e1c0a240` |
| 03 | [03-isel.md](03-isel.md) | 最小指令选择与栈帧骨架 | `llvm/test/CodeGen/RVMin/basic.ll` | `622c514cd75b` |
| 04 | [04-alu.md](04-alu.md) | 整数 ALU、比较、select、大常量 | `alu.ll` | `1f909e918cf5` |
| 05 | [05-control.md](05-control.md) | 分支与基本块布局 | `control.ll` | `74c05e46b568` |
| 06 | [06-stack.md](06-stack.md) | 栈帧、alloca、load/store | `stack.ll` | `d0c8955e4fc9` |
| 07 | [07-globals.md](07-globals.md) | 全局数据、字符串、`%hi/%lo` | `globals.ll` + `-r` 检查 | `413820d6f451` |
| 08 | [08-calls.md](08-calls.md) | 调用约定、递归、callee-saved | `calls.ll` | `36c57db80766` |
| 09 | [09-types.md](09-types.md) | i1/i8/i16/i64 类型合法化 | `types.ll` | `a81351206c08` |
| 10 | [10-libcalls.md](10-libcalls.md) | 乘除、位内建、软浮点 | `libcalls.ll` | `72abb22f2532` |
| 11 | [11-e2e.md](11-e2e.md) | 端到端、CFI、switch、测试体系 | `scripts/rvmin-e2e.sh`、`cfi.ll` | `8c8c83378834` |
| 12 | [12-run.md](12-run.md) | 链接与 QEMU 运行（ecall、运行时） | `scripts/rvmin-run.sh` | `4f71aa883223` |
| 13 | [13-m-extension.md](13-m-extension.md) | 可选 M 扩展与优化空间 | `m-ext.ll`、M 语料 | `b075676cca65` |
| 14 | [本索引](README.md) | 上篇验收：AC1–AC10 | `rvmin-verify-all.sh`（5 项） | `b3d4b6da5e43` |

### 下篇：从能跑到像样（15–23）

| 章 | 文档 | 主题 | 本章门禁 | tag |
| --- | --- | --- | --- | --- |
| 15 | [15-part2-overview.md](15-part2-overview.md) | 下篇导览与基线核对 | `scripts/rvmin-verify-all.sh` | `rvmin-ch15` |
| 16 | [16-large-frames.md](16-large-frames.md) | 大栈帧与 RegScavenger | 3 KiB 帧在 QEMU 运行 | `rvmin-ch16` |
| 17 | [17-tail-calls.md](17-tail-calls.md) | 尾调用与 musttail | 1e6 层尾递归不爆栈 | `rvmin-ch17` |
| 18 | [18-jump-tables.md](18-jump-tables.md) | 跳转表与间接跳转 | 8-case switch 只有 load+jalr | `rvmin-ch18` |
| 19 | [19-const-muldiv.md](19-const-muldiv.md) | 常量乘除强度削减 | `+m` 下 `x/7` 无 libcall | `rvmin-ch19` |
| 20 | [20-disassembler.md](20-disassembler.md) | 反汇编器 | 97 条语料往返 | `rvmin-ch20` |
| 21 | （规划中） | Zbb/Zbs | `clz/cpop/rev8` 单条 | `rvmin-ch21` |
| 22 | （规划中） | A 扩展 | `lr/sc` 程序在 QEMU 运行 | `rvmin-ch22` |
| 23 | （规划中） | 下篇验收 | 下篇 AC 表 | `rvmin-ch23` |

每个章末都有对应 tag：`git checkout rvmin-ch16` 即可看到该章结束时的完整代码。

## 每章固定包含什么

`AGENTS.md` 风格的教程要求每章都有：目标 / 背景 / 代码地图 / 关键实现 /
构建与验证命令 / 预期输出 / 常见坑 / 练习。各章用的标题不完全一样，对照如下
（“背景”通常由更具体的小节承担，例如数据流全景、指令格式、动机说明）。

| 章 | 背景 | 代码地图 | 验证命令与预期输出 | 常见坑 | 练习 |
| --- | --- | --- | --- | --- | --- |
| 00 | `## 背景` | `## 代码地图` | `## 构建与验证` | `## 常见坑` | `## 练习` |
| 01 | `## RV32I 的六种格式` | `## 指令清单` | `## 门禁与验证` | `## 常见坑` | `## 练习` |
| 02 | `## 为什么本章顺手写了 AsmParser` | `## 代码地图` | `## 门禁与验证` | `## 踩过的坑` | `## 练习` |
| 03 | `## 数据流全景` | `## 代码地图` | `## 门禁与验证` | `## 两个真实的坑` | `## 练习` |
| 04 | `## 目标`（第二段起） | `## 代码地图` | `## 门禁与验证` | `## 三个真实的坑` | `## 练习` |
| 05 | `## 目标`（第二段起） | `## 代码地图` | `## 门禁与验证` | `## 三个真实的坑` | `## 练习` |
| 06 | `## 目标`（第二段起） | `## 代码地图` | `## 门禁与验证` | `## 三个真实的坑` | `## 练习` |
| 07 | `## 目标`（第二段起） | `## 代码地图` | `## 门禁与验证` | `## 三个真实的坑` | `## 练习` |
| 08 | `## 目标`（第二段起） | `## 代码地图` | `## 验证` | `## 常见坑` | `## 练习` |
| 09 | `## 目标`（第二段起） | `## 代码地图` | `## 验证` | `## 常见坑` | `## 练习` |
| 10 | `## 目标`（第二段起） | `## 代码地图` | `## 验证` | `### 3. 一个必须踩的坑` | `## 练习` |
| 11 | `## 目标`（第二段起） | `## 代码地图` | `## 验证` | `## 关键实现` 里的三问题表 | `## 练习` |
| 12 | `## 目标`（第二段起） | `## 代码地图` | `## 验证` | `## 常见坑` | `## 练习` |
| 13 | `## 目标`（第二段起） | `## 代码地图` | `## 验证` | `## 关键实现` 里的“坑”注记 | `## 练习` |
| 15 | `## 背景` | `## 下篇路线` / 速查表 | `## 基线核对` | `## 常见坑` | `## 练习` |
| 16 | `## 背景` | `## 代码地图` | `## 构建与验证` | `## 常见坑` | `## 练习` |
| 17 | `## 背景` | `## 代码地图` | `## 构建与验证` | `## 常见坑` | `## 练习` |
| 18 | `## 背景` | `## 代码地图` | `## 构建与验证` | `## 常见坑` | `## 练习` |
| 19 | `## 背景` | `## 代码地图` | `## 构建与验证` | `## 常见坑` | `## 练习` |
| 20 | `## 背景` | `## 代码地图` | `## 构建与验证` | `## 常见坑` | `## 练习` |

> 第 14、23 章是验收章，本身即证据表，不套用八要素模板。

## 验收标准（计划 §6）

| ID | 标准 | 证据 | 状态 |
| --- | --- | --- | --- |
| AC1 | target 注册 | `llc --version \| grep -w rvmin` | 通过 |
| AC2 | 复用现有构建目录 | 只在 `builds/pr-ctpop` 配置（`LLVM_EXPERIMENTAL_TARGETS_TO_BUILD=RVMin`） | 通过 |
| AC3 | 编码正确 | `check-rvmin-encodings.sh`：85 条 RV32I + 12 条 M，与 `riscv32` 零差异 | 通过 |
| AC4 | 汇编可被参考汇编器接受 | 每个 `CodeGen/RVMin/*.ll` 的 RUN 行都有 `llvm-mc -triple=riscv32 -filetype=obj` 往返 | 通过 |
| AC5 | 目标文件合法 | `llvm-readelf -h` 报 `RISC-V`；`llvm-readelf -r`/`--unwind` 可读 | 通过 |
| AC6 | 标量 codegen 正确 | `llvm-lit llvm/test/CodeGen/RVMin`（10 个测试） | 通过 |
| AC7 | clang 端到端 | `rvmin-e2e.sh`：10 个 C 程序 × 4 个优化级 × 2 个特性集 = 80 个对象 | 通过 |
| AC8 | 改动范围 | `git diff --stat` 仅 `llvm/lib/Target/RVMin/**`、`llvm/test/{CodeGen,MC}/RVMin/**` | 通过 |
| AC9 | 教程完整 | 本索引 + 每章的目标/背景/地图/验证/坑/练习 | 通过 |
| AC10 | 可选运行时 | `rvmin-run.sh`：RV32I 与 RV32I+M 各跑一次，输出一致 | 通过 |

## 一键复现

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop

cmake -S "$W/llvm" -B "$B" -DLLVM_EXPERIMENTAL_TARGETS_TO_BUILD=RVMin
ninja -C "$B" llc clang llvm-mc llvm-objdump llvm-readelf llvm-objcopy FileCheck

"$B/bin/llc" --version | grep -w rvmin
"$B/bin/llvm-lit" -s "$W/llvm/test/CodeGen/RVMin" "$W/llvm/test/MC/RVMin"
"$W/llvm/lib/Target/RVMin/scripts/check-rvmin-encodings.sh" "$B/bin"
"$W/llvm/lib/Target/RVMin/scripts/rvmin-e2e.sh" "$B/bin"
"$W/llvm/lib/Target/RVMin/scripts/rvmin-run.sh" "$B/bin"
```
