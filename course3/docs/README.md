# rmach 课程索引

第三课：从零实现一台支持 M/U 特权级、Sv32 分页和定时器中断的 RV32 机器，
并在它上面启动一个最小内核。第一课见
[`../../llvm/lib/Target/RVMin/docs/README.md`](../../llvm/lib/Target/RVMin/docs/README.md)，
第二课见 [`../../course2/docs/README.md`](../../course2/docs/README.md)。

| 章 | 文档 | 主题 | 本章门禁 | tag |
| --- | --- | --- | --- | --- |
| 00 | [本索引](README.md) | 导览与骨架 | `rmach --version`；verify 输出 `0 checks` | `c3-ch00` |
| 01 | [01-bare-metal.md](01-bare-metal.md) | 裸机镜像与启动 | entry/PT_LOAD 与 `llvm-readelf` 一致 | `c3-ch01` |
| 02 | [02-csr-privilege.md](02-csr-privilege.md) | CSR 文件与特权级 | 22 条 CSR 自测；U 访问特权 CSR 非法 | `c3-ch02` |
| 03 | [03-traps-mret.md](03-traps-mret.md) | 精确陷阱与 `mret` | 6 种 M 态 cause + 3 种 U 态 cause | `c3-ch03` |
| 04 | [04-clint-timer.md](04-clint-timer.md) | CLINT 与定时器中断 | `mcause=0x80000007`；向量模式 | `c3-ch04` |
| 05 | （规划中） | MMIO 总线与 UART | stdout 与 golden 逐字节一致 | `c3-ch05` |
| 06 | （规划中） | Sv32 分页 | 恒等/非恒等映射一致；页错误 12/13/15 | `c3-ch06` |
| 07 | （规划中） | U 态与系统调用 | 第二课 `demo` 作为 U 程序运行 | `c3-ch07` |
| 08 | （规划中） | 上下文与两进程调度 | 两进程轮转、退出码回传 | `c3-ch08` |
| 09 | （规划中） | 调试与可观测性 | trace 条数 == `minstret`；页表 dump 正确 | `c3-ch09` |
| 10 | （规划中） | 第三课验收 | C3-AC1–9 全绿 | `c3-ch10` |

第 00 章只是骨架，因此没有行为检查；`scripts/course3-verify.sh` 打印
`0 checks` 是预期结果，不是漏测。

## 每章固定包含

目标 / 背景 / 代码地图 / 关键实现 / 构建与验证 / 预期输出 / 常见坑 / 练习。
“常见坑”只记录真实踩到的问题。

## 构建与验证

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop

cmake -S "$W/course3" -B "$B/course3" -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C "$B/course3"
"$W/course3/scripts/course3-verify.sh" "$B/bin" "$B/course3/rmach"
```
