# RVMin 教程索引

RVMin 是一个从零写的 LLVM 后端：以 RV32I 为基线，按章节逐步加入 M/A/Zbb/Zbs
整数扩展与 F/D 浮点扩展。本目录是配套教程：每章一个提交，每章都能独立构建、
测试、复现。

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
| 14 | [本索引](README.md) | 上篇验收：AC1–AC10 | `rvmin-verify-all.sh`（当时 5 项，现 6 项） | `b3d4b6da5e43` |

### 下篇：从能跑到像样（15–23）

| 章 | 文档 | 主题 | 本章门禁 | tag |
| --- | --- | --- | --- | --- |
| 15 | [15-part2-overview.md](15-part2-overview.md) | 下篇导览与基线核对 | `scripts/rvmin-verify-all.sh` | `rvmin-ch15` |
| 16 | [16-large-frames.md](16-large-frames.md) | 大栈帧与 RegScavenger | 3 KiB 帧在 QEMU 运行 | `rvmin-ch16` |
| 17 | [17-tail-calls.md](17-tail-calls.md) | 尾调用与 musttail | 1e6 层尾递归不爆栈 | `rvmin-ch17` |
| 18 | [18-jump-tables.md](18-jump-tables.md) | 跳转表与间接跳转 | 8-case switch 只有 load+jalr | `rvmin-ch18` |
| 19 | [19-const-muldiv.md](19-const-muldiv.md) | 常量乘除强度削减 | `+m` 下 `x/7` 无 libcall | `rvmin-ch19` |
| 20 | [20-disassembler.md](20-disassembler.md) | 反汇编器 | 97 条语料往返 | `rvmin-ch20` |
| 21 | [21-zbb-zbs.md](21-zbb-zbs.md) | Zbb/Zbs | `clz/cpop/rev8` 单条 | `rvmin-ch21` |
| 22 | [22-atomics.md](22-atomics.md) | A 扩展与 AtomicExpand | 20 条 A 编码与 `riscv32` 一致；`lr/sc` 程序在 QEMU 运行 | `rvmin-ch22` |
| 23 | [23-acceptance.md](23-acceptance.md) | 下篇验收 | P2-AC1–10 表 + 未做清单 | `rvmin-ch23` |

### 第三篇：clang 一等 target（24–28）

| 章 | 文档 | 主题 | 本章门禁 | tag |
| --- | --- | --- | --- | --- |
| 24 | [24-clang-target-triple.md](24-clang-target-triple.md) | `Triple::rvmin` 与连带白名单 | `Triple.RVMin`；lit 24/24；`rvmin-verify-all.sh` 6/6 | `rvmin-ch24` |
| 25 | [25-clang-targetinfo.md](25-clang-targetinfo.md) | clang TargetInfo（cc1 可用） | `clang -cc1 -emit-obj` + 2 个 lit | `rvmin-ch25` |
| 26 | [26-clang-driver.md](26-clang-driver.md) | clang driver（`--target=rvmin`） | `-###`/object/asm 门禁 + 2 个 lit | `rvmin-ch26` |
| 27 | [27-clang-run.md](27-clang-run.md) | clang 驱动链接与 QEMU 运行 | `rvmin-clang-run.sh` 6 组程序 | `rvmin-ch27` |
| 28 | [28-acceptance.md](28-acceptance.md) | 第三篇验收：80 对象矩阵 | `rvmin-clang-matrix.sh` 80/80 | `rvmin-ch28` |

### 实验课：TableGen DSL 语义公开化（29–39）

| 章 | 文档 | 主题 | 本章门禁 | tag |
| --- | --- | --- | --- | --- |
| 29 | [29-register-info-dsl.md](29-register-info-dsl.md) | 从 emitter 反推 `-gen-register-info` 的字段与名字契约 | `dsl-surface.py --check`；门禁 7/7 | `rvmin-ch29` |
| 30 | [30-instr-info-dsl.md](30-instr-info-dsl.md) | 把契约提取器推广到 `-gen-instr-info`：按后端配置、投影矩阵、派生改写 | 两个 `--check` + 门禁 7/7 | `rvmin-ch30` |
| 31 | [31-asm-writer-dsl.md](31-asm-writer-dsl.md) | 第三个后端 `-gen-asm-writer`：同一份 `.td` 的三种投影 | 三个 `--check` + 门禁 7/7 | `rvmin-ch31` |
| 32 | [32-cross-backend-status.md](32-cross-backend-status.md) | 跨后端 status 矩阵：一张表看完同一份 `.td` 的投影差异 | `--status-matrix --check` + 门禁 7/7 | `rvmin-ch32` |
| 33 | [33-field-effect.md](33-field-effect.md) | 字段影响面：一个字段牵动哪些后端与读取点 | `--effect TSFlags --check` + 门禁 7/7 | `rvmin-ch33` |
| 34 | [34-asm-matcher-dsl.md](34-asm-matcher-dsl.md) | 第四个后端 `-gen-asm-matcher`：非指令 schema 与 42 个 project 字段 | `--backend asm-matcher --check` + 门禁 7/7 | `rvmin-ch34` |
| 35 | [35-upstream-contract.md](35-upstream-contract.md) | 契约前移：把方法论写进 `llvm/docs/TableGen/BackEnds.md` | 上游文档锚点/路径检查 + 门禁 7/7 | `rvmin-ch35` |
| 36 | [36-dag-isel-dsl.md](36-dag-isel-dsl.md) | 第五个后端 `-gen-dag-isel`：pattern schema 与 135 个 derive 字段 | `--backend dag-isel --check` + 门禁 7/7 | `rvmin-ch36` |
| 37 | [37-fast-isel-dsl.md](37-fast-isel-dsl.md) | 第六个后端 `-gen-fast-isel`：派生类字段与 2 个 derive+project | `--backend fast-isel --check` + 门禁 7/7 | `rvmin-ch37` |
| 38 | [38-disassembler-dsl.md](38-disassembler-dsl.md) | 第七个后端 `-gen-disassembler`：target 自定义 `Inst` 与契约边界 | `--backend disassembler --check` + 门禁 7/7 | `rvmin-ch38` |
| 39 | [39-dsl-acceptance.md](39-dsl-acceptance.md) | 实验课验收：七后端总表、七条判据与方法边界 | `--summary --check` + 门禁 7/7 | `rvmin-ch39` |

> 勘误：`rvmin-ch28` 的 AC7 漏掉了“未支持扩展必须报错”，修复与审计记录见
> [28-acceptance.md 的勘误节](28-acceptance.md#勘误ch28-审计)，tag `rvmin-ch28a`。

### 第四篇：浮点（40–42）

| 章 | 文档 | 主题 | 本章门禁 | tag |
| --- | --- | --- | --- | --- |
| 40 | [40-f-extension.md](40-f-extension.md) | F 扩展、FPR32、ILP32F 调用约定与 ABI 校验 | `f-extension.s` + `f-ext.ll`；门禁 7/7 | `rvmin-ch40` |
| 41 | [41-d-extension.md](41-d-extension.md) | D 扩展、FPR64、ILP32D 调用约定与 f64 常量/select | `d-extension.s` + `d-ext.ll`；门禁 7/7 | `rvmin-ch41` |
| 42 | [42-fd-acceptance.md](42-fd-acceptance.md) | 第四篇验收：F/D 全链路、8 项门禁、192 对象矩阵与 QEMU | `rvmin-verify-all.sh` 8/8 + `rvmin-clang-matrix.sh` | `rvmin-ch42` |

### 第五篇：压缩指令（43–45）

| 章 | 文档 | 主题 | 本章门禁 | tag |
| --- | --- | --- | --- | --- |
| 43 | [43-c-extension-mc.md](43-c-extension-mc.md) | RV32C 的 MC 层：9 种 16 位格式、35 条指令、RVC fixup 与 16 位解码 | `c-extension.s`；编码 35 条 + 反汇编/混排往返；门禁 8/8 | `rvmin-ch43` |

> 44（压缩代码生成）与 45（第五篇验收）在后续章节加入本表。

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
| 21 | `## 背景` | `## 代码地图` | `## 构建与验证` | `## 常见坑` | `## 练习` |
| 22 | `## 背景` | `## 代码地图` | `## 构建与验证` | `## 常见坑` | `## 练习` |
| 40 | `## 背景：特性与 ABI 是两件事` | `## 代码地图` | `## 构建与验证` | `## 常见坑` | `## 练习` |
| 41 | `## 背景：RV32 上的 double 为什么更麻烦` | `## 代码地图` | `## 构建与验证` | `## 常见坑` | `## 练习` |
| 43 | `## 背景：16 位指令为什么是另一套格式` | `## 代码地图` | `## 构建与验证` | `## 常见坑` | `## 练习` |

> 第 14、23、28、39、42 章是验收章，本身即证据表，不套用八要素模板。

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
