# 第 08 章：上下文切换与两进程调度

## 目标

让同一台机器交替运行两个 U 态程序：

- `rmach --user a.elf --user b.elf` 加载两个用户镜像；
- 内核为每个进程建**独立页表**，两个进程可以使用相同的虚拟地址；
- CLINT 定时器每 4000 条指令触发一次中断，内核在陷阱返回路径上做轮转；
- 每个进程的 31 个寄存器、`pc`、`satp` 独立保存与恢复；
- `SYS_exit` 收集每个进程的退出码，全部退出后机器停机；
- `--stats` 输出指令数、定时器次数、`ecall` 次数；`--dump-procs` 读内核发布的进程表。

门禁从 17 项增加到 **21 项**，`--selftest` 仍是 32 项。

本章不做优先级、睡眠队列、阻塞 I/O 和内核栈切换；它们放进练习。

## 背景：为什么切换点在陷阱返回

第 03 章的 `trap.S` 已经把所有 31 个寄存器保存到内核栈，并在返回前逐项恢复。
调度器不需要另写一份汇编，只需要在 `handle_trap()` 里做三件事：

1. 把当前 `trap_frame` 抄进当前进程的 `regs[32]`；
2. 选出下一个进程，把它的 `regs[32]` 抄回 `trap_frame`；
3. 写 `mepc`、`satp`、`mscratch`，然后让 `trap.S` 继续走原来的恢复 + `mret`。

```text
U 态进程 A
   │ timer interrupt
   ▼
trap_entry：换栈，保存 x1/x3..x31 到 trap_frame
   │
   ▼
handle_trap：A.regs = frame；选 B；frame = B.regs
   │          mepc = B.pc；satp = B.satp；mscratch = B.sp
   ▼
trap_entry：恢复寄存器，换栈，mret
   │
   ▼
U 态进程 B
```

这条路径同时覆盖三种事件：

| 事件 | `mepc` 处理 | 是否切换 |
| --- | --- | --- |
| 定时器中断 | 保存**当前** `pc`，回来继续执行 | 是 |
| `SYS_write` | `mepc + 4`，跳过 `ecall` | 否 |
| `SYS_exit` | 不需要，进程不会再回来 | 是 |

“定时器保存当前 pc、syscall 加 4”是本章最容易写错的一处：中断发生在指令执行
**之前**，那条指令还没退休；`ecall` 已经退休，`mepc` 指向它自己。

### 为什么每个进程要有自己的页表

两个程序都链接在 `0x10000`，都使用 `0x7ff00000` 作为用户栈。如果共用一个页表，
它们会互相覆盖。独立页表让**相同的虚拟地址**映射到**不同的物理页**：

| 进程 | 镜像物理基址 | 栈物理基址 | 根表 |
| --- | --- | --- | --- |
| 0 | `0x80400000` | `0x80420000` | `roots[0]` |
| 1 | `0x80440000` | `0x80460000` | `roots[1]` |

`satp` 只在 U 态生效；内核在 M 态继续用物理地址读写两个进程的页表和栈。切换
`satp` 不会影响内核自己的取指。

### 为什么量子不能太小

第 04 章的测试把 `mtimecmp` 设成很小的步长；本章如果照搬 200 条指令，会观察到
“打印一行 A 之后机器卡死”。原因不是调度器坏了，而是**陷阱处理本身比量子还长**：

```text
trap_entry 保存 31 个寄存器   ~40 条
handle_trap 保存/恢复两个进程  ~70 条
proc_switch_to + arm_timer     ~80 条
trap_entry 恢复 + mret         ~40 条
------------------------------------------------
一次切换约 230+ 条指令
```

量子设为 200 时，`mret` 回到 U 态的下一条指令立刻又满足 `mtime >= mtimecmp`，
于是永远在处理中断，用户程序一条也执行不了。本章把量子设为 **4000**，留出足够的
用户指令预算，同时仍能在 300/200 行的输出中产生近 30 次抢占。

## 代码地图

| 文件 | 作用 |
| --- | --- |
| `course3/kernel/boot.h` | `BOOT_MAX_PROCS`、进程 arena 布局、`boot_result` |
| `course3/kernel/proc.{h,c}` | 进程表、`proc_init()`、轮转选择、结果发布 |
| `course3/kernel/kmain.c` | 定时器分支、`write/exit` 分发、量子与 `arm_timer()` |
| `course3/kernel/mmu.{h,c}` | 每进程根表、`mmu_init(pid, ...)`、`user_translate(pid, ...)` |
| `course3/kernel/trap.S` | 不变的保存/恢复；`enter_user` 设 `MPIE=1` |
| `course3/kernel/string.c` | freestanding `memset/memcpy` |
| `course3/mach/machine.{h,cpp}` | `setUser(pid, ...)`、结果页、定时器/ecall 计数 |
| `course3/mach/main.cpp` | 可重复 `--user`、`--stats`、`--dump-procs` |
| `course3/tests/proc-a.S` | 300 行 `A\n`，退出码 7 |
| `course3/tests/proc-b.S` | 200 行 `B\n`，退出码 0 |
| `course3/scripts/course3-verify.sh` | 21 checks |

## 关键实现

### 1. 启动协议：一个 `boot_info` 数组

`--user` 现在可以重复。每个镜像对应一个 `boot_info`，连续放在
`BOOT_INFO_ADDR`（176 字节一个，4 KiB 页足够 8 个）：

```c
struct boot_info {
  uint32_t magic;       // 0x524d4143
  uint32_t user_entry;
  uint32_t user_sp;
  uint32_t nseg;
  struct boot_seg seg[8];
};
```

`rmach` 进入内核时：

- `a0 = BOOT_INFO_ADDR`，指向数组第一个元素；
- `a1 = nproc`，本章是 1 或 2。

内核不再假设“只有一个用户镜像”，而是 `boots[pid]`。没有 `--user` 时
`a1=0`，`boots[0].nseg==0`，`kmain` 仍走第 05 章的 hello 回退路径。

每个进程的物理布局由 `boot.h` 的宏算出，不新增结构体字段：

```c
#define BOOT_USER_BASE 0x80400000u
#define BOOT_USER_STRIDE 0x00040000u      // 每进程 256 KiB
#define BOOT_USER_STACK_OFFSET 0x00020000u
#define BOOT_USER_IMAGE_BASE(pid) (BOOT_USER_BASE + (pid) * BOOT_USER_STRIDE)
#define BOOT_USER_STACK_PHYS(pid) (BOOT_USER_IMAGE_BASE(pid) + 0x20000u)
```

`Machine::setUser(pid, image)` 按页归并 ELF 段（第 07 章的规则不变），把页表
写到 `boots[pid]`，并把用户物理页放在该进程的 arena 里。`BOOT_USER_SIZE` 从
`0x40000` 扩到 `0x80000`，`Machine::reset()` 一次性映射整个 arena。

### 2. `struct proc` 与初始化

```c
struct proc {
  uint32_t regs[32]; // 与 trap_frame.x 同布局
  uint32_t pc;
  uint32_t satp;
  int state;         // RUNNABLE / RUNNING / EXITED
  int exit_code;
};
```

`proc_init(boots, count)` 对每个 `pid`：

1. `mmu_init(pid, &boots[pid])` 建独立根表；
2. `pc = boots[pid].user_entry`；
3. `satp = mmu_satp(pid)`；
4. `regs[2] = boots[pid].user_sp`，其余寄存器清零；
5. `state = RUNNABLE`。

`proc_start()` 把进程 0 标成 RUNNING，调用
`enter_user(procs[0].pc, procs[0].regs[2], procs[0].satp)`。

### 3. 定时器分支与轮转

`kmain` 在进入 U 态前做三件事：

```c
csrw_mtvec((uint32_t)trap_entry);
csrw_mie(MIE_MTIE);   // bit 7：machine timer interrupt enable
arm_timer();          // mtimecmp = mtime + 4000
proc_start();
```

`enter_user` 把 `mstatus` 设为 `MPIE=1`（bit 7）、`MPP=U`：

```asm
li      t0, 0x80
csrw    mstatus, t0
mv      sp, a1
mret
```

`mret` 会做 `MIE = MPIE = 1`，所以 U 态运行在开中断状态。第 07 章这里写的是
`mstatus=0`，那时没有定时器，U 态一直关中断；本章必须改成 `0x80`。

定时器分支：

```c
if (cause == 0x80000007u) {
  procs[current].pc = epc;        // 中断前的那条指令
  save_current(tf);               // tf->x -> procs[current].regs
  procs[current].state = PROC_RUNNABLE;
  arm_timer();
  proc_switch_to(tf, proc_pick_next());
  return;
}
```

`proc_pick_next()` 从 `current+1` 开始找第一个非 EXITED 的进程，最多绕一圈。
`proc_switch_to()` 做四件事：

```c
current = next;
for (r = 0; r < 32; ++r) tf->x[r] = procs[current].regs[r];
csrw_mepc(procs[current].pc);
csrw_satp(procs[current].satp);
csrw_mscratch(procs[current].regs[2]);
```

注意最后一个：`trap.S` 返回时的 `sp` 来自 `mscratch`，不是 `tf->x[2]`。
如果只恢复 `tf` 里的 `sp` 而忘了写 `mscratch`，下一个进程会继续用上一个进程的
用户栈，输出看起来正常但栈会互相踩。

### 4. `SYS_write` 与 `SYS_exit`

`write` 与第 07 章相同，只是 `user_translate()` 多了一个 `pid`，走当前进程的
根表。它在 M 态完成整段拷贝，而陷阱入口已经把 `mstatus.MIE` 清零，所以一次
`write` 的字节不会被定时器打断——这正是门禁能断言“每行完整”的原因。

`exit` 不再直接停机：

```c
procs[current].exit_code = tf->x[10] & 0xff;
procs[current].state = PROC_EXITED;
proc_last_exit = procs[current].exit_code;
proc_publish();
proc_switch_to(tf, proc_pick_next());
```

`proc_pick_next()` 返回 `-1` 时，`proc_switch_to()` 先 `proc_publish()` 再
`finish(proc_last_exit)`。因此宿主退出码是**最后一个退出进程**的退出码，不是
最大或求和；`--dump-procs` 里仍能看到每个进程自己的退出码。

本章两个程序：A 打印 300 行后退出 7，B 打印 200 行后退出 0。A 的工作量更大，
通常最后退出，所以宿主退出码是 7。

### 5. `--stats` 与 `--dump-procs`

`Machine` 增加两个计数器：

```cpp
if ((cause & kInterruptBit) && (cause & 0x7fffffffu) == 7)
  ++timer_ticks_;
if (cause == 8)
  ++ecall_count_;
```

`--stats` 把它们写到 **stderr**，不污染 stdout golden：

```text
rmach: stats: instructions=156326 timer_ticks=29 ecalls=502
```

`--dump-procs` 读内核发布在 `0x800fa000` 的 `boot_result`：

```c
struct boot_result {
  uint32_t magic;   // 0x52534c54 ("RSLT")
  uint32_t nproc;
  struct boot_proc_result proc[BOOT_MAX_PROCS];
};
```

内核在初始化、每次退出、全部退出时调用 `proc_publish()`。`rmach` 停机后逐字段
读取并打印；结果页在 `Machine::reset()` 里映射为普通 RAM。

### 6. freestanding 的 `memset/memcpy`

`proc_init()` 里的清零循环和 `struct proc` 的复制会被 clang 识别成
`memset/memcpy` 调用。内核没有 libc，链接会报：

```text
ld.lld: error: undefined symbol: memset
```

`kernel/string.c` 提供这两个函数，和 `uart.c/mmu.c` 一起链进内核。它们不是
“为了好看”，而是 `-ffreestanding` 下 clang 仍可能生成的 libcall。

## 构建与验证

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop

ninja -C "$B/course3"
"$W/course3/scripts/course3-verify.sh" "$B/bin" "$B/course3/rmach"
```

门禁 21 项，第 08 章新增 4 项：

1. **两进程输出**：stdout 恰好 300 行 `A` + 200 行 `B`，没有其他字节；
2. **定时器证据**：`--stats` 的 `timer_ticks >= 4`，且 `ecalls=502`
   （300 write + 200 write + 2 exit）；
3. **进程表**：`--dump-procs` 打印 `nproc 2`，进程 0 `EXITED exit=7`、
   进程 1 `EXITED exit=0`；
4. **交错执行**：B 的第一行出现在 A 的第 300 行之前，A 的第一行出现在 B 的
   第 200 行之前；串行执行会失败。

第 05 章的 hello 回退、第 06 章的页表 dump、第 07 章的 demo/跨页 write 仍在
同一脚本里通过；定时器现在也会打断单进程 demo，但轮转回自身，输出不变。

## 预期输出

```text
$ rmach --version
rmach 0.8 (chapter 08)

$ rmach --kernel kernel.elf --user proc-a --user proc-b --stats
A
A
...
B
...
rmach: stats: instructions=156326 timer_ticks=29 ecalls=502
rmach: halted at 0x80000804 after 156326 instructions (exit 7)

$ rmach --kernel kernel.elf --user proc-a --user proc-b --dump-procs
A
...
B
...
nproc 2
proc 0 state=EXITED exit=7 pc=0x00010020
proc 1 state=EXITED exit=0 pc=0x00010020

$ rmach --kernel kernel.elf --user proc-a --user proc-b --dump-boot
...
user0 entry 0x00010000
user0 load vaddr=0x00010000 filesz=0x00000038 memsz=0x00000038 flags=0x5 align=0x1000
user0 load vaddr=0x00010038 filesz=0x00000002 memsz=0x00000002 flags=0x4 align=0x1000
user1 entry 0x00010000
user1 load vaddr=0x00010000 filesz=0x00000038 memsz=0x00000038 flags=0x5 align=0x1000
user1 load vaddr=0x00010038 filesz=0x00000002 memsz=0x00000002 flags=0x4 align=0x1000
```

指令数、tick 数和 A/B 的具体交错顺序会随工具链与量子变化；门禁只断言
行数、`timer_ticks` 下界、`ecalls` 总数和进程表内容。

## 常见坑

1. **量子小于陷阱处理成本**。量子 200 时，一次切换要 230+ 条指令，`mret` 后
   立刻再次中断，用户程序永远跑不动。现象是“只打印一行就卡死”，实测后把量子
   改成 4000。
2. **`enter_user` 仍写 `mstatus=0`**。第 07 章没有定时器，这没问题；本章 U 态
   必须 `MIE=1` 才能被抢占。正确写法是 `mstatus = MPIE = 0x80`，`mret` 会把
   `MIE` 置 1。
3. **切换时只改 `tf->x[2]`，忘了 `mscratch`**。`trap.S` 的返回 `sp` 来自
   `mscratch`；不写它就会把 B 的返回地址/栈混在一起，输出可能仍“能跑”，但
   栈已经串了。
4. **定时器也加 4**。中断的 `epc` 是还没执行的那条指令，必须原样恢复；只有
   `ecall` 才 `epc+4`。把两者混用会丢一条用户指令或重复一次系统调用。
5. **退出码取最大值或求和**。本章语义是“最后退出进程的退出码”；每个进程的
   退出码单独放在 `--dump-procs` 里。门禁同时检查两者。
6. **忘了发布结果页**。`finish()` 停机后 `rmach` 才读 `0x800fa000`；如果
   `proc_publish()` 在 `finish()` 之后，结果页还是空的，`--dump-procs` 会打印
   `(no process table)`。
7. **两个进程共用一个根表**。它们都链接在 `0x10000`，共用页表会让后加载的
   镜像覆盖前一个；正确做法是每个 `pid` 一个 `roots[pid]`，`satp` 在切换时更新。
8. **内核链接缺 `string.c`**。`-O2` 会把清零循环变成 `memset`，把结构体复制
   变成 `memcpy`；没有这两个符号时链接直接失败。
9. **`struct proc` 变长后没检查布局**。`proc_switch_to()` 的数组拷贝依赖
   `regs[32]` 在结构体开头；如果在 `regs` 前加字段，汇编/编译器算出的偏移会
   变，必须重新核对 `4*i` 的帧布局。
10. **把 `proc_publish()` 放进每次切换**。它写 2 个进程 × 4 个字段，放在
    高频路径上会显著抬高陷阱成本；本章只在初始化、退出和最终停机时发布。

## 练习

1. 支持 4 个进程：把 `BOOT_MAX_PROCS` 改成 4，arena 扩到 `0x100000`，
   验证轮转顺序和结果页数组。
2. 给进程加优先级：`proc_pick_next()` 先扫高优先级，再扫低优先级；用 A/B/C
   三个程序验证低优先级不会饿死。
3. 实现 `SYS_sleep`（`a7=101`）：进程进入 SLEEPING，定时器到期后唤醒；
   需要每进程一个 `wake_at` 字段。
4. 让 `SYS_read` 阻塞：没有输入时进程进入 BLOCKED，另一个进程继续跑；
   `--input` 到达后唤醒。
5. 给每个进程独立的**内核栈**：把 `mscratch` 换成每进程内核栈顶，验证嵌套
   陷阱和 `--dump-procs` 的 `pc`。
6. 在 `--trace` 里打印 `pid` 和 `satp`，统计每个进程的指令数，和
   `--stats` 的 `instructions` 总和对照。
7. 实现 `SYS_wait`（`a7=95`）：父进程等待子进程退出并拿到它的退出码；
   需要进程树和 `EXITED` 状态的回收。
8. 把 `proc_publish()` 的结果页改成只读映射，让用户程序无法伪造进程表；
   同时给 `--dump-procs` 加 magic/长度校验。
