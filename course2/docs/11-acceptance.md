# 第 11 章：第二课验收

## 目标

用一张**可复现**的表收口第二课：C2-AC1–8 每条都有命令、有输出、有归档位置。
本章不写新功能；它证明前 11 章的东西现在仍然一起工作。

验收通过的定义：

- `course2-verify.sh` 退出码 0，打印 **196 checks**；
- `rsim --selftest` 打印 **107 checks, 0 failures**；
- 8 个构建在 rsim 与 QEMU 下 stdout/退出码全等；
- `git diff --stat rvmin-ch23..rsim-ch11` 只出现 `course2/**`。

## 背景：为什么验收单独成章

前 10 章每章都有自己的门禁，但“每章绿”不等于“合起来对”：

| 只做单章门禁会漏掉 | 本章怎么抓 |
| --- | --- |
| 后来的 `--isa` 默认值把早期程序挡掉 | 8 构建统一带 `--isa` 重跑 |
| 统计/跟踪改了 `executed_` 语义 | 三口径交叉校验（ch10） |
| 扩展指令破坏了 RV32I 路径 | 97 条语料 + QEMU 差分 |
| 文档索引与实际 tag 漂移 | tag 清单 + 索引逐行核对 |

此外，本章把“8 个构建”从规划里的一句话变成门禁里的真实循环，
避免验收文档和脚本各说各话。

## 代码地图

| 交付物 | 位置 | 规模（`git diff --stat rvmin-ch23..rsim-ch11`） |
| --- | --- | --- |
| 模拟器源码 | `course2/rsim/` | 17 个源/头文件 + `insn.def` |
| 门禁 | `course2/scripts/course2-verify.sh` | 698 行，196 项检查 |
| 单测 | `course2/rsim/selftest.cpp` | 107 条指令向量 |
| 取证程序 | `course2/tests/` | 8 个 `.s` + golden transcript |
| 文档 | `course2/docs/` | 00–11 共 12 篇（索引 + 11 章） |
| 合计 | 44 个文件 | 5,773 insertions |

第一课目录（`llvm/lib/Target/RVMin/**`）在本分支零改动；语料与运行时只读复用。

## 关键实现：C2-AC 逐条证据

| ID | 标准 | 门禁里的证据 | 结果 |
| --- | --- | --- | --- |
| C2-AC1 | ELF 加载正确 | `compare()`：7 个构建的入口 + PT_LOAD 与 `llvm-readelf -hl` 逐字段一致 | ✅ |
| C2-AC2 | 译码覆盖 | 85 条 RV32I + 12 条 M 语料逐行与 `llvm-mc -disassemble` 相同（97/97） | ✅ |
| C2-AC3 | 基础执行 | `demo` 输出 `rvmin: ok`，退出 0，`t0=0` | ✅ |
| C2-AC4 | 差分一致 | 8 构建 × {rsim, QEMU}，stdout + 退出码全等 | ✅ |
| C2-AC5 | 跟踪可信 | trace 地址 ⊆ `llvm-objdump -d`，文本 == `--disas`，行数 == 执行数 | ✅ |
| C2-AC6 | 调试可用 | 脚本会话 transcript 与 `tests/debug-session.golden` 逐字节一致 | ✅ |
| C2-AC7 | 错误可解释 | 非法指令 `mcause=2`、ebreak `mcause=3`、非对齐精确 epc、未知 syscall 127 | ✅ |
| C2-AC8 | 范围可控 | `git diff --stat rvmin-ch23..HEAD` 仅 `course2/**` | ✅ |

### 8 个构建（C2-AC4 的完整名单）

| # | 构建 | 来源 | 关键 flag | 门禁位置 |
| --- | --- | --- | --- | --- |
| 1 | `demo` | `rt/demo.c` | `-O2` | ch05 差分循环 |
| 2 | `muldiv_m` | `rt/muldiv.c` | `-O2 -mattr=+m` | ch05 差分循环 |
| 3 | `large_frame` | `rt/large-frame.c` | `-O2` | ch05 差分循环 |
| 4 | `large_frame_o0` | `rt/large-frame.c` | `-O0` | ch05 差分循环 |
| 5 | `switch` | `rt/switch.c` | `-O2` | ch05 差分循环 |
| 6 | `tail` | `rt/tail.c` | `-O2`，10⁶ 次尾调用 | ch05 差分循环 |
| 7 | `atomics` | `rt/atomics.c` | `-mattr=+a`，`--isa=rv32i_a` | ch05 差分循环 |
| 8 | `muldiv_i` | `rt/muldiv.c` | 无 M，走 `rvmin-rt.S` libcall；`--isa=rv32i` | ch09 |

`atomics` 在 `--isa=rv32i_a` 下运行；默认掩码是 `RV32I|RV32M`，不显式打开 A
会得到预期中的 `instruction not enabled by --isa`。

### 统计口径（ch10 的交叉验证）

`tail` 的实测值：

```text
instructions 18010059
jalr 2001006
```

门禁同时验证：`instructions` == 各行之和 == `after N instructions`；
`demo` 的 top-5 与独立聚合 `--trace` 的结果逐字相同。

## 构建与验证

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop

cmake -S "$W/course2" -B "$B/course2" -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C "$B/course2"
"$B/course2/rsim" --selftest
"$W/course2/scripts/course2-verify.sh" "$B/bin" "$B/course2/rsim"

# 第一课回归（确认第二课没有反向污染）
"$W/llvm/lib/Target/RVMin/scripts/rvmin-verify-all.sh" "$B/bin"
```

## 预期输出

```text
$ rsim --version
rsim 0.11 (chapter 11)

$ rsim --selftest | tail -1
selftest: 107 checks, 0 failures

$ course2-verify.sh $B/bin $B/course2/rsim
196 checks
（退出码 0，耗时约 7 s）

$ git tag --list 'rsim-*'
rsim-ch00 … rsim-ch11
```

按章回看：ch00–ch02 是加载与内存，ch03–ch05 是指令与运行时，
ch06–ch08 是可观测性与陷阱，ch09–ch10 是扩展与统计，ch11 是这张表。

## 常见坑

1. **拿“最后一次手动跑绿”当验收**。门禁的检查项会随章节增长
   （135 → 186 → 190 → 196）；验收必须重新跑脚本并核对计数。
2. **忘了 `atomics` 需要 `--isa=rv32i_a`**。默认掩码不含 A，
   症状是“atomics 程序在 rsim 下非法指令”，而 QEMU 正常。
3. **`large_frame -O0` 被优化级别的缓存骗了**。两个构建必须用不同的
   输出文件名（`large_frame` 与 `large_frame_o0`），否则后者覆盖前者，
   差分只剩一个。
4. **QEMU 的 `.bss` 页零填充怪癖**。`atomics.c` 的两个全局变量故意带
   非零初值（落在 `.data`），否则 6.2 的用户态加载器会把与 `.text` 共享的
   整页清零，程序在第一条指令就崩——这是第一课 ch22 踩过的坑。
5. **范围检查看工作树而不是提交**。`git status` 里的 `labs/`、`riscv-demo/`
   是未跟踪的本地目录，不属于本课；验收看
   `git diff --stat rvmin-ch23..rsim-ch11`。
6. **tag 只在本地**。`--tags` 会尝试推所有历史 tag 并卡住；始终
   `git push origin course2/rv32-emulator rsim-ch11`。

## 未做清单（有意留在第二课之外）

| 项 | 原因 | 建议去向 |
| --- | --- | --- |
| C 扩展（16-bit 译码） | 会改变“每条指令 4 字节”的取指模型 | 第二课附录 A 或第三课第 0 部分 |
| GDB RSP | 本机 `gdb` 不支持 `riscv:rv32`（实测） | 附录 B，门禁用自带客户端 |
| 特权态 / MMU（`satp`、Sv32） | 第二课边界是用户态 | 第三课主线 |
| 流水线 / cache 模型 | ch10 只做“周期 = 指令数” | 第三课或独立专题 |
| 汇编器 / 链接器 | 工作量独立成课 | 第四课 |

## 练习

1. 把 `course2-verify.sh` 的 `196 checks` 拆成按章计数，输出一张表。
2. 给门禁加 `--list-checks`，只打印检查名，便于对照本文档。
3. 用 `git bisect` 在 `rsim-ch00..rsim-ch11` 上找一个“故意注入”的 bug，
   验证每章门禁确实能定位回归。
4. 把 8 个构建扩到 `-O0..-O3` × {无 M, +M}，观察 `muldiv` 的 libcall 消失点。
5. 按“未做清单”任选一项，写一页开课提案：前置、章数、验收标准。
