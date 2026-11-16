# 第 10 章：第三课验收

## 目标

把 C3-AC1–9 从“计划里的表格”变成“可复现的证据”：

- `course3-verify.sh` 输出 **26 checks**；
- `--selftest` 仍是 **32 checks**；
- 第二课 `course2-verify.sh` 仍是 **196 checks**；
- 第一课 `rvmin-verify-all.sh` 输出 **ALL GATES PASS**；
- `git diff --name-only rsim-ch11..c3-ch10` 只出现 `course3/**`；
- 本文逐条给出命令、期望与实测结果。

`rmach` 的版本在本章收口为 `1.0`。

## 验收命令

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop

cmake -S "$W/course3" -B "$B/course3" -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C "$B/course3"

"$W/course3/scripts/course3-verify.sh" "$B/bin" "$B/course3/rmach"
"$W/course2/scripts/course2-verify.sh" "$B/bin" "$B/course2/rsim"
"$W/llvm/lib/Target/RVMin/scripts/rvmin-verify-all.sh" "$B/bin"

"$B/course3/rmach" --selftest
git diff --name-only rsim-ch11..c3-ch10 | grep -v '^course3/' ; test $? -eq 1
```

最后一行 `grep` 没有输出、返回 1，表示差异全部落在 `course3/`。

## 实测结果

```text
$ rmach --version
rmach 1.0 (chapter 10)

$ rmach --selftest
selftest: 32 checks, 0 failures

$ course3-verify.sh ...
26 checks

$ course2-verify.sh ...
196 checks

$ rvmin-verify-all.sh ...
PASS 1/6: target registered
...
PASS 6/6: 128 llc runs with -verify-machineinstrs
ALL GATES PASS
```

## C3-AC1–9 逐条证据

### C3-AC1 裸机启动正确

| 证据 | 命令/位置 |
| --- | --- |
| 内核 entry == `0x80000000` | 第 01 章 readelf 对比门禁（check 1） |
| PT_LOAD 与 `llvm-readelf -hl` 一致 | 同上，Python 逐项比较 `vaddr/filesz/memsz` |
| `_start` 第一条 trace 地址正确 | 第 01 章门禁检查首行 `$2 == 80000000`（check 3） |
| 内核跑到 test finisher | 第 01 章门禁检查退出码 0（check 2） |

### C3-AC2 特权切换正确

| 证据 | 命令/位置 |
| --- | --- |
| U `ecall` → M handler → `mret` 回 U | `tests/trap-u.s`（check 6） |
| 31 个 GPR 与进入前逐项相等 | `tests/trap-allregs.S`（check 25）：用 `sp` 做数组基址，ecall 前后各存 31 个寄存器并逐字比较 |
| 反向验证 | 故意删除 handler 的 `lw t0/lw t1` 后，同一测试以退出码 1 失败 |

`trap-allregs.S` 的做法：把 `x1..x31` 各设一个唯一常量，以 `sp` 为基址写入
`before[32]`；`ecall` 进入 M 态 handler，handler 只保存/恢复自己用到的
`t0/t1`；`mret` 回到 U 后写入 `after[32]`，再循环比较两份快照。`x0` 恒为 0，
不参与比较。

### C3-AC3 陷阱精确

| 证据 | 命令/位置 |
| --- | --- |
| M 态 6 种 `(cause,epc,tval)` | `tests/traps.s`（check 5）：U ecall=8、M ecall=11、非法=2、取指非对齐=0、load 非对齐=4、store 非对齐=6 |
| U 态 3 种 cause | `tests/trap-u.s`（check 6）：ecall=8、ebreak=3、非法=2 |
| 页错误 12/13/15 | 第 06 章 `mmu-fault`（13）与 `--selftest` 的 Sv32 向量（12/15） |

### C3-AC4 定时器中断

| 证据 | 命令/位置 |
| --- | --- |
| `mcause=0x80000007`、向量模式 | `tests/timer.s`（check 7） |
| 未 arm 不触发 | `tests/timer-off.s`（check 8） |
| 两进程被抢占 | 第 08 章 `--stats` 的 `timer_ticks >= 4`（check 19） |
| `mtime` 单调 | `timer.s` 内逐次比较；`--stats` 的 tick 数随指令数增长 |

### C3-AC5 UART 可信

| 证据 | 命令/位置 |
| --- | --- |
| `hello, rmach\n` 逐字节一致 | 第 05 章 `cmp` 门禁（check 9） |
| `--input` 回显到第一个换行 | `tests/uart-echo.c`（check 10） |
| test word 高半退出码 | `tests/finisher.s` 写 `0x00075555`，宿主退出 7（check 11） |
| LSR 位语义 | `uart-echo` 轮询 `LSR.DR`；`uart_putc` 轮询 `LSR.THRE` |

### C3-AC6 Sv32 正确

| 证据 | 命令/位置 |
| --- | --- |
| 恒等/非恒等映射输出一致 | `tests/mmu-run.c` 输出两行 `U ok`（check 12） |
| `--dump-pagetable` 叶子正确 | 重定位页与 UART 页（check 13）；第 07 章 demo 页（check 17） |
| `U=0` 拒绝 U 访问 → cause 13 | `tests/mmu-fault.c`（check 14） |
| 取指 12 / store 15 / `A/D=0` | `--selftest` 的 10 条 Sv32 向量（check 4） |
| PTE.PPN `<<2` | 第 06 章文档“常见坑”与 selftest 手算向量 |

### C3-AC7 进程可用

| 证据 | 命令/位置 |
| --- | --- |
| 两进程轮转 | `proc-a.S`/`proc-b.S`（check 18）：300 行 A + 200 行 B |
| 无输出交错损坏 | 每行只允许 `A`/`B`，逐行计数（check 18） |
| 量子到点切换 | `--stats` 的 `timer_ticks >= 4`（check 19） |
| 退出码回传 | `--dump-procs` 显示 `0:exit=7`、`1:exit=0`（check 20） |
| 非线性串行 | A/B 首行都出现在对方结束之前（check 21） |

### C3-AC8 三课闭环

| 证据 | 命令/位置 |
| --- | --- |
| 第二课 `demo` 作为 U 程序 | 第 07 章门禁：stdout `rvmin: ok\n`、退出 0（check 15） |
| 跨页 `write` | `tests/user-crosspage.S`（check 16） |
| `muldiv_i`（rvmin，无 M 扩展）作为 U 程序 | 第 10 章门禁：stdout `rvmin: ok\n`、退出 0（check 26） |

`demo` 与 `muldiv_i` 都由第一课的 rvmin 后端编译、第二课的 rvmin 运行时链接，
再交给第三课的 `rmach` 在 U 态执行——三课在同一命令里闭环。

### C3-AC9 范围可控

```text
$ git diff --name-only rsim-ch11..c3-ch10 | grep -v '^course3/'
$ echo $?
1
```

没有输出且返回 1，说明从第二课收口 `rsim-ch11` 到第三课收口 `c3-ch10` 的
所有改动都在 `course3/**`。`course2/**` 与 `llvm/lib/Target/RVMin/**` 冻结。

## 门禁计数一览

| 章 | checks | 新增内容 |
| --- | --- | --- |
| 00 | 0 | 骨架 |
| 01 | 3 | entry/PT_LOAD/首条 trace |
| 02 | 4 | 32 条 selftest |
| 03 | 6 | M/U 精确陷阱 |
| 04 | 8 | 定时器与未 arm |
| 05 | 11 | hello/echo/finisher |
| 06 | 14 | Sv32、页表 dump、cause 13 |
| 07 | 17 | demo、跨页 write、demo 页表 |
| 08 | 21 | 两进程、stats、进程表、交错 |
| 09 | 24 | trace==stats==minstret、CSR dump、U 态 PA |
| 10 | 26 | 31 个 GPR、muldiv_i |

## 未做清单

本章明确不做，留给后续课程或练习：

- S 态、`sret`、`medeleg/mideleg`；
- PLIC 与外部设备中断；
- 4 MiB 超级页、TLB、`sfence.vma`；
- 多 hart、原子指令的跨 hart 语义；
- 浮点、DMA、动态链接、文件系统；
- QEMU system 模式的差分测试（仅保留可选设备冒烟）。

这些边界不影响 C3-AC1–9；它们已在第 06、08、09 章的“练习”里给出入口。
