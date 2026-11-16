# 第 05 章：系统调用与运行时

## 目标

打通“程序 → 宿主”的最后一段：`ecall` 不再只是“停下”，而是真正的系统调用。

- `a7 = 64`：`write(fd, buf, len)`，把 guest 内存里的字节写到宿主 stdout/stderr；
- `a7 = 93`：`exit(status)`，退出码原样传给宿主；
- 其他调用号：停住并退出 **127**，而不是假装成功；
- `rsim --run` 的返回值等于 guest 的退出码。

本章结束时，第一课的 5 个程序在 `rsim` 与 QEMU 下 **stdout 逐字节相同、退出码相同**。

## 背景：Linux 用户态 ABI

第一课的运行时只用了两个调用（`llvm/lib/Target/RVMin/scripts/rt/start.S:26-31`）：

```asm
        addi    a0, zero, 1              // fd = stdout
        addi    a7, zero, 64             // SYS_write
        ecall
        ...
        addi    a7, zero, 93             // SYS_exit
        ecall
```

约定很简单：**调用号在 `a7`，参数在 `a0/a1/a2`，返回值回到 `a0`**。
模拟器要做的事情也只有三件：读调用号、动宿主 I/O、把结果写回 `a0`。

和真正的内核相比，我们省掉的是“一切”：没有进程、没有文件描述符表、没有信号。
课程只需要 `write` 和 `exit`，范围写在章末。

## 代码地图

| 文件 | 作用 |
| --- | --- |
| `rsim/syscall.cpp` | `Cpu::ecall()`：64/93/未知 |
| `rsim/cpu.cpp` | `step()` 里 `ecall` 的位置与 `pc` 推进 |
| `rsim/main.cpp` | `--run` 返回 guest 退出码 |
| `tests/exit-code.s` | `exit(42)` 必须变成宿主 42 |
| `tests/unknown-syscall.s` | `a7=999` 必须变成 127 |

## 关键实现

### 1. `ecall` 也是普通指令

```cpp
case Insn::ECALL:
  if (!ecall(at))   // exit(2) 结束循环
    return false;
  break;            // 其余情况继续走统一的 pc = next
```

第一版写成 `return ecall(at);`，于是 `write` 返回后 `pc` 没有前进，程序在
`ecall` 上原地打转——输出已经打印出来了，但最后撞上步数上限、退出码 126。
这个 bug 是门禁抓到的，写进“常见坑”。

### 2. `write` 一个字节一个字节地读

```cpp
for (uint32_t i = 0; i < len; ++i)
  std::fputc(load8(buf + i, at), out);
```

用 `load8` 而不是直接 `memcpy` 宿主缓冲区：缓冲区是 guest 的地址，
越界就必须是**精确的 load trap**（`epc` 指向这条 `ecall`），而不是宿主段错误。

### 3. 退出码只有 8 位

`exit(status)` 的 status 与 POSIX 一致，只保留低 8 位（`x[10] & 0xff`）。
`--run` 直接 `return cpu.exitCode()`，所以 shell 里 `$?` 就是 guest 的返回值。

### 4. guest 的 stdout 和模拟器的诊断必须分开

- guest `fd=1` → 宿主 stdout；
- guest `fd=2` → 宿主 stderr；
- 模拟器自己的“halted at ...”摘要 → **stderr**。

否则第 05 章的差分测试会被自己的摘要污染：`rsim` 的输出永远比 QEMU 多一行。
这是本章唯一一个“为了可测性而改变输出通道”的设计决定。

### 5. 未知调用：127，而不是 -ENOSYS

QEMU 的行为是返回 `-ENOSYS`（`a0 = -38`）然后继续跑，程序最终可能正常退出 0。
我们选择停下并退出 127，理由：**课程里一个没实现的调用必须响**。
差异实测如下，文档不隐藏：

```text
$ qemu-riscv32-static ./unknown_syscall
rvmin: ok
$ echo $?
0

$ rsim --run unknown_syscall
rsim: trap: unknown ecall a7=999 at 0x0001004c
$ echo $?
127
```

## 构建与验证

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop
"$W/course2/scripts/course2-verify.sh" "$B/bin" "$B/course2/rsim"
```

第 05 章新增 7 项检查（总数 120 → 127）：

1. `demo / muldiv_m / large_frame / switch / tail`：与 `qemu-riscv32-static`
   的 **stdout 逐字节相同**（`cmp -s`），**退出码相同**；
2. `tests/exit-code.s`：`rsim` 与 QEMU 都退出 42；
3. `tests/unknown-syscall.s`：`rsim` 退出 127 且 stderr 有 `unknown ecall a7=999`。

门禁要求宿主机有 `qemu-riscv32-static`，否则直接失败（不允许“跳过即通过”）。

## 预期输出

```text
$ rsim --run demo
rvmin: ok
$ echo $?
0

$ rsim --run demo 2>/dev/null | od -c | head -2
0000000   r v m i n :   o k \n
```

`tail` 需要更大的步数上限（1e6 次尾调用 ≈ 1800 万条指令）：

```text
$ rsim --run tail --max-steps 50000000
rvmin: ok
$ echo $?
0
```

## 常见坑

1. **`ecall` 处理完忘了推进 `pc`**。`return ecall(at)` 把统一收尾跳过去了，
   程序在 `ecall` 上无限循环；症状很迷惑——输出是对的、退出码是 126。
2. **把摘要留在 stdout**。差分测试第一版直接失败在“多了一行 halted”，
   而不是程序行为；诊断信息一律 stderr。
3. **`write` 直接 `memcpy`**。guest 的缓冲区可能非法，必须先过 `load8`，
   否则宿主进程收到 SIGSEGV，看起来像模拟器自己崩了。
4. **未知调用照抄 QEMU 的 `-ENOSYS`**。那会让“没实现”在课程里静默通过；
   我们选择 127，并把差异写进本章。
5. **退出码 126/127 与模拟器自身错误码撞车**。约定：125 = 加载失败、
   126 = 模拟器 trap、127 = guest 用了未知调用；guest 自己 `exit(126)` 仍返回 126，
   文档里注明“两者无法区分，但课程脚本不会同时触发”。

## 练习

1. 实现 `read`（`a7=63`），让程序可以从 stdin 读；想想为什么它比 `write` 难测。
2. 把未知调用的行为改成 `a0 = -ENOSYS` 并继续执行，然后设计一个能区分
   “QEMU 兼容模式”和“严格模式”的命令行开关。
3. 统计 `demo` 产生的 `write` 次数与字节数，和 `strace -c` 的结果对比。
4. 让 `write` 支持 `fd=0`（读）并解释为什么这在内核里是另一个系统调用。
