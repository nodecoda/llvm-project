# rsim 课程索引

第二课：从零实现 RV32I 模拟器与调试器。第一课见
[`../../llvm/lib/Target/RVMin/docs/README.md`](../../llvm/lib/Target/RVMin/docs/README.md)。

| 章 | 文档 | 主题 | 本章门禁 | tag |
| --- | --- | --- | --- | --- |
| 00 | [本索引](README.md) | 导览与骨架 | `rsim --version`；verify 输出 `0 checks` | `rsim-ch00` |
| 01 | [01-elf-loader.md](01-elf-loader.md) | ELF32 加载器 | 入口/段表与 `llvm-readelf -hl` 一致 | `rsim-ch01` |
| 02 | [02-memory-stack.md](02-memory-stack.md) | 内存、栈与启动 | `.bss` 零填充、sp 对齐、越界 trap | `rsim-ch02` |
| 03 | [03-decode-exec.md](03-decode-exec.md) | 译码与执行 RV32I | 97/97 条语料与 `llvm-mc` 一致；`--selftest` 65 条 | `rsim-ch03` |
| 04 | [04-control-flow.md](04-control-flow.md) | 控制流与精确异常 | `jalr` LSB 清零、`epc` 与 `llvm-objdump` 对齐 | `rsim-ch04` |
| 05 | （规划中） | 系统调用与运行时 | `demo` 输出 `rvmin: ok`，退出码 0 | `rsim-ch05` |
| 06 | （规划中） | 跟踪与自反汇编 | trace ⊆ `llvm-objdump -d` | `rsim-ch06` |
| 07 | （规划中） | 断点、单步与脚本调试 | 脚本 transcript 与 golden 一致 | `rsim-ch07` |
| 08 | （规划中） | 陷阱模型与 ebreak | `mcause=2`、`mepc` 正确 | `rsim-ch08` |
| 09 | （规划中） | M/Zbb/A 可选指令 | 与 QEMU 差分一致 | `rsim-ch09` |
| 10 | （规划中） | 指令统计与“性能” | `jalr` 计数 ≥ 1e6 | `rsim-ch10` |
| 11 | （规划中） | 第二课验收 | C2-AC1–8 全绿 | `rsim-ch11` |

第 00 章只是骨架，因此没有行为检查；`scripts/course2-verify.sh` 打印
`0 checks` 是预期结果，不是漏测。

## 每章固定包含

目标 / 背景 / 代码地图 / 关键实现 / 构建与验证 / 预期输出 / 常见坑 / 练习。
“常见坑”只记录真实踩到的问题。

## 构建与验证

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop

cmake -S "$W/course2" -B "$B/course2" -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C "$B/course2"
"$W/course2/scripts/course2-verify.sh" "$B/bin" "$B/course2/rsim"
```
