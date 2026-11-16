# 第 07 章：进入 U 态与系统调用

## 目标

让 `rmach` 第一次运行**真正的用户程序**：不是测试里手写的机器码 blob，而是第二课
的 `demo`——它由第一课的 rvmin 后端编译，第二课的 `rsim` 曾经运行过，现在交给第三课
的 `rmach`，在 Sv32 页表和 U 特权级下运行。

本章要打通四段：

- `rmach --user <elf>` 解析用户 ELF，把每个 `PT_LOAD` 复制到物理 arena，并把
  **页级**段表写进 `boot_info`；
- 内核 `mmu_init()` 按 `boot_info` 建 Sv32 页表，映射用户代码/数据、用户栈和 UART；
- `enter_user()` 设 `satp/mepc/mscratch/mstatus`，用 `mret` 进入 U 态；
- `ecall` 分发：`a7=64` 写 stdout，`a7=93` 退出；`write` 的缓冲区可以跨页。

门禁从 14 项增加到 **17 项**，`--selftest` 仍是 32 项。三课在这里闭环：

```text
rvmin 后端  →  用户 ELF  →  rmach 的 Sv32 页表  →  M 态内核的 ecall 处理
（第一课）                    （第三课）                （本章）
```

本章不做 `fork/exec`、文件系统、`read`、信号和进程隔离；这些留给第 08 章与练习。

## 背景：为什么需要启动协议

内核不应该自己解析 ELF。原因很实际：

- ELF 解析在第二课已经验证过（`course2/rsim/elf.cpp`），第三课再写一份只会多一处
  漂移；
- 内核链接在 `0x80000000`，没有 `malloc`，也不该在 M 态做文件 I/O；
- `rmach` 是宿主程序，读文件、校验、复制都方便，出错能直接返回 125。

所以边界定在 `boot_info` 这一页：

```text
rmach（宿主 C++）                         kernel（裸机 C）
  loadElf(user)                             kmain(boot_info *)
  copy PT_LOAD pages  ── boot_info ──>      mmu_init()
  write 0x800fb000                          enter_user()
```

内核只信任这一页，不看 ELF header。

### 为什么段表是“页级”的

ELF 的 `PT_LOAD` 按 `p_vaddr/p_memsz` 划分，**不同段可以落在同一个 4 KiB 页**。
`demo` 就是这样：

```text
user load vaddr=0x00010000 filesz=0x000004f4 memsz=0x000004f4 flags=0x5  R+X
user load vaddr=0x000104f4 filesz=0x00000020 memsz=0x00000020 flags=0x4  R
user load vaddr=0x00010518 filesz=0x0000003c memsz=0x0000003c flags=0x6  R+W
```

三段都在 `0x10000` 这一页里。页表只有一套权限位，所以这一页的最终权限必须是三者的
**并集**：`R|W|X`。如果按 ELF 段逐条 `map_page()`，后写的段会覆盖前一条：

- 先 `R+X`，再 `R`，再 `R+W`；
- 最终页变成 `R+W`，没有 `X`；
- `mret` 到 `0x10000` 的第一条取指就是 cause 12（instruction page fault）。

这是本章真实踩到的坑。修复方式是在 `Machine::setUser()` 里先按页归并：

```cpp
page->flags |= seg.flags;   // 并集，而不是覆盖
```

`boot_seg` 因此记录的是**一个页的放置**，不是 ELF 段：

```c
struct boot_seg {
  uint32_t vaddr;   // 页对齐的虚拟地址
  uint32_t phys;    // 页对齐的物理地址
  uint32_t filesz;  // 通常 4096
  uint32_t memsz;   // 通常 4096
  uint32_t flags;   // PF_X=1, PF_W=2, PF_R=4 的并集
};
```

`static_assert(sizeof(boot_seg) == 20)` 与
`static_assert(sizeof(boot_info) == 16 + 8 * 20)` 把 C 与 C++ 的布局钉死；
`boot_info` 是逐字节写进 RAM 的，不能有隐式 padding 漂移。

### 为什么用 `mscratch` 换栈

U 态程序有自己的 `sp`，陷阱入口不能直接用它——那会把用户栈当成内核栈。RISC-V 的
标准做法是让 `mscratch` 保存**内核栈指针**，入口第一条指令交换：

```asm
csrrw sp, mscratch, sp      # sp = kernel sp, mscratch = user sp
addi  sp, sp, -128          # trap_frame
...
csrrw sp, mscratch, sp      # 反向交换，恢复两边
mret
```

进入 U 态前，`enter_user()` 把 `mscratch` 设成 `0x80100000`（内核栈顶）。如果忘了
这一步，第一次 `ecall` 会把 `sp` 换成 0，随后保存寄存器直接 access fault。

### 为什么 `write` 需要软件 walk

内核在 M 态，`Machine::translate()` 对 M 态恒等映射。`a1` 是用户**虚拟**地址，
内核不能直接 `*(char *)a1`——那会读到物理地址 `a1`。两个选择：

1. 临时打开 `mstatus.MPRV`，让 M 态按 U 权限翻译一次 load；
2. 在 `mmu.c` 里写一个只读的软件 walk。

本章选 2：逻辑显式、可单步、不引入 `MPRV/SUM/MXR` 的状态组合。代价是每次拷贝都要
走两遍页表，但教学实现里这是可接受的。

## 代码地图

| 文件 | 作用 |
| --- | --- |
| `course3/kernel/boot.h` | `boot_info`/`boot_seg` 协议与 arena 常量 |
| `course3/kernel/kernel.h` | `trap_frame`、CSR 写助手、`finish/fail` |
| `course3/kernel/kmain.c` | 校验 magic、建表、进 U 态、`ecall` 分发 |
| `course3/kernel/mmu.{h,c}` | 静态页表池、`map_page()`、`user_translate()` |
| `course3/kernel/trap.S` | `trap_entry` 的换栈/保存/恢复，`enter_user` 的 `mret` |
| `course3/kernel/uart.{h,c}` | 从 `kmain` 抽出的轮询控制台 |
| `course3/kernel/exit.c` | `finish()`/`fail()` 写 test word |
| `course3/mach/machine.{h,cpp}` | `setUser()`：页级归并、复制、写 `boot_info` |
| `course3/mach/main.cpp` | `--user <elf>`；`--dump-boot` 增加 `user ` 前缀 |
| `course3/tests/user-crosspage.S` | 24 字节缓冲区跨页的 `write` |
| `course3/scripts/course3-verify.sh` | 17 checks |

## 关键实现

### 1. `setUser()`：按页归并

```cpp
for (const rsim::Segment &seg : user.segments) {
  const uint32_t first = seg.vaddr & ~0xfffu;
  const uint32_t last = (seg.vaddr + seg.memsz - 1) & ~0xfffu;
  for (uint32_t page_va = first; page_va <= last; page_va += 4096) {
    boot_seg *page = findPage(page_va);
    if (page == nullptr) {
      page = &info.seg[info.nseg++];
      page->vaddr = page_va;
      page->phys = cursor;
      page->filesz = page->memsz = 4096;
      page->flags = 0;
      cursor += 4096;
    }
    page->flags |= seg.flags;          // 并集
    // 把 [max(seg.vaddr, page_va), min(seg.vaddr+filesz, page_va+4096))
    // 的字节复制到 page->phys + (va - page_va)
  }
}
```

三个边界：

- `cursor` 从 `0x80400000` 按页递增，遇到 `BOOT_USER_STACK_PHYS` 就报错，防止用户
  镜像压到栈；
- 空段（`memsz == 0`，例如纯 `.bss`）跳过；
- `flags == 0` 的段按只读处理（内核 `seg_flags()` 里补 `R`）。

结果写进 `0x800fb000`，`a0` 设成这个物理地址：

```cpp
x[10] = kBootInfo; // kmain(boot_info *)
```

### 2. 内核建表：静态池 + 固定映射

`mmu.c` 不分配内存，只有两块静态数组：

```c
static uint32_t root[1024] __attribute__((aligned(4096)));
static uint32_t tables[8][1024] __attribute__((aligned(4096)));
```

`MAX_TABLES=8` 对本章足够：`demo` 一个代码页、四个栈页、一个 UART 页，只需要
几个二级表。`map_page()` 在根表项无效时从池里取一张表并清零；池耗尽返回 `-1`，
`kmain` 用 `fail(2)` 报出来。

权限映射：

| ELF flags | PTE |
| --- | --- |
| `R` | `V|R|U|A` |
| `W` | `V|R|W|D|U|A`（`W=1` 必须同时 `R=1`） |
| `X` | `V|X|U|A` |
| 并集 | 上述按位或 |

固定映射还有两段：用户栈 `0x7fefc000..0x7ff00000` → `0x80420000..0x80424000`
（4 页，`R/W/U/A/D`），以及 UART 页 `0x10000000` → 自身（`R/W/U/A/D`）。
UART 页在本章的 `write` 路径里其实由 M 态内核直接访问，但保留 U 映射让第 06 章的
观察和第 09 章的直接控制台都成立。

### 3. `trap_entry`：31 个寄存器 + `x2` 槽

帧大小 128 字节，`x[i]` 在 `4*i`：

| 偏移 | 寄存器 | 偏移 | 寄存器 |
| --- | --- | --- | --- |
| 4 | `ra` | 68 | `a7` |
| 8 | `sp`（用户栈） | 72–108 | `s2`–`s11` |
| 12 | `gp` | 112–124 | `t3`–`t6` |
| 16 | `tp` | 20 | `t0` |
| 24–28 | `t1`,`t2` | 32–36 | `s0`,`s1` |
| 40–64 | `a0`–`a6` | | |

`x0` 不保存（永远为 0）；`x2` 槽存的是交换前的用户 `sp`，只为调试可读。
保存顺序有一个小技巧：先用 `sw t0, 20(sp)` 保住 `t0`，再用它读 `mscratch`，
否则用户 `t0` 会被破坏。

调用约定：`a0=tf`、`a1=cause`、`a2=epc`、`a3=tval`。C 函数可以随意用 caller-saved
寄存器，返回后汇编逐项恢复；`trap_frame` 里被修改的 `a0` 就是系统调用返回值。

### 4. `enter_user()`：一条 `mret` 的四件事

```asm
enter_user:
        csrw    satp, a2
        csrw    mepc, a0
        li      t0, 0x80100000
        csrw    mscratch, t0
        csrw    mstatus, zero
        mv      sp, a1
        mret
```

1. `satp`：`MODE=1 | root >> 12`；M 态继续用物理地址，所以下一条指令不受影响；
2. `mepc`：用户入口（虚拟地址，本章是 `0x10000`）；
3. `mscratch`：内核栈顶，供下一次陷阱换栈；
4. `mstatus = 0`：`MPP=00` 表示返回 U 态，`MIE=0` 表示进 U 后先关中断；
   `mret` 会做 `priv=MPP`、`MIE=MPIE`、`MPIE=1`、`pc=mepc`。

`mstatus=0` 不是“随便清零”：`MPP` 两位必须显式是 0。若沿用复位值或误写
`MPP=M`，`mret` 会回到 M 态，用户程序看起来“跑了”，但每次 `ecall` 的 cause 是
11 而不是 8。

### 5. `ecall` 分发与返回

```c
if (cause != 8) fail(4);          // 本章只接受 EcallFromU
const uint32_t number = tf->x[17]; // a7
if (number == 64) {                // SYS_write
  uint32_t fd = tf->x[10], buf = tf->x[11], len = tf->x[12];
  if (fd != 1 && fd != 2) {
    tf->x[10] = 0xffffffffu;       // -EBADF，但继续运行
    csrw_mepc(epc + 4);
    return;
  }
  for (uint32_t i = 0; i < len; ++i) {
    uint32_t pa = user_translate(buf + i);
    if (pa == 0) fail(5);
    uart_putc(*(volatile unsigned char *)pa);
  }
  tf->x[10] = len;                 // 返回写入字节数
  csrw_mepc(epc + 4);
  return;
}
if (number == 93) finish(tf->x[10] & 0xffu); // SYS_exit
fail(6);                           // 未知系统调用
```

ABI 与第二课的 `rvmin-rt` 完全一致：`a0/a1/a2` 是参数，`a7` 是编号，返回值放
`a0`。`ecall` 本身已经把 `mepc` 指向 `ecall`，所以返回用户前必须 `epc + 4`；
忘了这一步会反复执行同一条 `ecall`。

`write` 逐字节调用 `user_translate()`，所以缓冲区跨页也能正确：

```asm
msg:  .ascii "0123456789abcdefghijklmn"   # 从页偏移 4090 开始
```

`user_translate()` 的检查是 `V|R|U|A` 全为 1，返回 `PA | (va & 0xfff)`；
任何一项不满足返回 0，内核用 `fail(5)` 停机。它不做 `D` 检查，因为写路径走的是
内核的 UART，不是用户页。

### 6. 失败路径的编号

`fail(N)` 先打印 `rmach kernel: fail NN\n`，再写 `0x7777`，宿主退出码 1：

| N | 含义 |
| --- | --- |
| 1 | `boot_info` 为空或 magic 不是 `0x524d4143` |
| 2 | `mmu_init()` 失败（静态表池耗尽） |
| 3 | `enter_user()` 返回了（不应发生） |
| 4 | 陷阱 cause 不是 8（不是 U 态 `ecall`） |
| 5 | `write` 的用户地址无法翻译 |
| 6 | 未知系统调用号 |

成功路径完全静默：`demo` 只输出自己的 `rvmin: ok\n`。

### 7. 没有 `--user` 时保留第 05 章门禁

`kmain` 在 magic 校验后检查 `nseg == 0`：

```c
if (boot->nseg == 0) {
  uart_puts("hello, rmach\n");
  finish(0);
}
```

`rmach` 在没有 `--user` 时调用 `setUser(rsim::Image{})`，于是 `boot_info` 合法但
`nseg=0`。这样第 05 章的 `hello, rmach\n` 门禁不用改内核就能继续跑，也让读者可以
先用 `--kernel` 单独调试内核。

## 构建与验证

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop

ninja -C "$B/course3"
"$W/course3/scripts/course3-verify.sh" "$B/bin" "$B/course3/rmach"
```

门禁 17 项，第 07 章新增 3 项：

1. **demo 作为 U 程序**：用第一课链路构建（`clang -S -emit-llvm` → `llc -march=rvmin`
   → `llvm-mc -triple=rvmin` → `ld.lld -T rt/link.ld`），`rmach --user` 运行，
   stdout 逐字节等于 `rvmin: ok\n`，退出码 0；
2. **跨页 `write`**：`user-crosspage.S` 的 24 字节缓冲区从页偏移 4090 开始，
   stdout 逐字节等于 `0123456789abcdefghijklmn`；
3. **页表 dump**：`--dump-pagetable` 必须出现
   `leaf va=0x00010000 pa=0x80400000 ... flags=VRWXUAD`（并集）、UART 页
   `VRWUAD`，以及最后一页用户栈 `va=0x7feff000 pa=0x80423000`。

`--selftest` 仍是 32 条；本章没有新增 CSR/MMU 单元向量，证据全部是端到端的。

## 预期输出

```text
$ rmach --version
rmach 0.7 (chapter 07)

$ rmach --kernel kernel.elf --user demo
rvmin: ok
rmach: halted at 0x8000055c after 17123 instructions (exit 0)

$ rmach --kernel kernel.elf --user cross
0123456789abcdefghijklmn
rmach: halted at 0x8000055c after 13819 instructions (exit 0)

$ rmach --kernel kernel.elf --user demo --dump-pagetable
rvmin: ok
satp 0x80080001 mode=1 asid=0x000 ppn=0x080001
leaf va=0x00010000 pa=0x80400000 pte=0x201000df level=0 flags=VRWXUAD
leaf va=0x10000000 pa=0x10000000 pte=0x040000d7 level=0 flags=VRWUAD
leaf va=0x7fefc000 pa=0x80420000 pte=0x201080d7 level=0 flags=VRWUAD
leaf va=0x7fefd000 pa=0x80421000 pte=0x201084d7 level=0 flags=VRWUAD
leaf va=0x7fefe000 pa=0x80422000 pte=0x201088d7 level=0 flags=VRWUAD
leaf va=0x7feff000 pa=0x80423000 pte=0x20108cd7 level=0 flags=VRWUAD

$ rmach --kernel kernel.elf --user demo --dump-boot
entry 0x80000000
load vaddr=0x80000000 filesz=0x00000760 memsz=0x00000760 flags=0x5 align=0x1000
...
user entry 0x00010000
user load vaddr=0x00010000 filesz=0x000004f4 memsz=0x000004f4 flags=0x5 align=0x1000
...
```

页表数组地址、`pte` 值与指令数会随编译器/链接布局变化；门禁只断言 stdout、叶子
映射和退出码。`--dump-boot` 的 `user ` 前缀表示这是 `--user` 镜像的段。

## 常见坑

1. **按 ELF 段而不是按页建表**。`demo` 的 `R+X`、`R`、`R+W` 三段同页，逐段
   `map_page()` 会让最后一组权限覆盖前面，页变成 `RW`、没有 `X`，U 态第一条取指
   cause 12。必须按页归并 flags（本章真实踩坑）。
2. **`mstatus` 没设 `MPP=U`**。`mstatus=0` 才是 U；从复位值直接 `mret` 会回 M 态，
   `ecall` 的 cause 变成 11，`handle_trap` 的 `cause != 8` 分支直接 `fail(4)`。
3. **忘了 `epc + 4`**。`mepc` 指向 `ecall` 本身；不前进就永远重复同一次调用，
   表现为死循环或 `--max-steps`。
4. **`mscratch` 没设内核栈**。第一次陷阱的 `csrrw` 会把 `sp` 换成 0，保存 `ra`
   立即 access fault；栈顶固定 `0x80100000`，与 `start.S` 一致。
5. **`a7` 的偏移写错**。`trap_frame` 是 `x[i]` 在 `4*i`：`a0=40`、`a7=68`。
   写 `tf->x[7]` 会读到 `t2`。
6. **用户栈映射区间差一页**。`BOOT_USER_SP=0x7ff00000`，栈大小 `0x4000`，
   第一页是 `0x7fefc000`；循环条件必须是 `va < BOOT_USER_SP`。
7. **`write` 只翻译首地址**。用户缓冲区可以跨页；`user_translate(buf)` 一次然后
   连续拷 `len` 字节，会在页边界后读到物理上不连续的内存。必须逐字节翻译。
8. **内核用 `gp` 访问静态数组**。`start.S` 不初始化 `gp`；`KFLAGS` 里的
   `-msmall-data-limit=0` 必须保留，否则 `root/tables` 会走 `gp=0` 直接 fault。
9. **把 `boot_seg` 当 ELF 段用**。它的 `vaddr/phys/filesz/memsz` 都是页粒度；
   内核若按 ELF 段语义再切一次，会把同页的多个段重复映射。
10. **`finish()` 的退出码截断**。`SYS_exit` 只取 `a0 & 0xff`；宿主 test word 的
    高 16 位才是退出码，低 16 位必须是 `0x5555`。

## 练习

1. 增加 `SYS_read`（`a7=63`）：从 UART RBR 读一个字节到用户缓冲区，返回 1；
   EOF 返回 0。注意目标页必须是 `W|U|A|D`。
2. 把 `write` 的错误从 `fail(5)` 改成返回 `-EFAULT`，让用户程序自己处理坏指针。
3. 用 `mstatus.MPRV` 实现一次 `write` 拷贝，替换 `user_translate()`，比较两条路径的
   页错误语义。
4. 给 `write` 增加 `fd=2` 的 stderr 分流：本章两者都写 UART，可以加一个
   `--stderr <file>` 让机器把 fd 2 写到独立文件。
5. 在用户栈上构造 `argc/argv/envp`，让 `demo` 能读到启动参数；需要扩展
   `boot_info` 或在内核里写栈。
6. 把 `tables[8][1024]` 换成按需的 free-list，支持超过 8 个二级表的大用户程序。
7. 给 `setUser()` 加 `--dump-boot` 的页级视图（`page va=... phys=... flags=...`），
   对照 `--dump-pagetable` 的叶子，确认两边一一对应。
8. 实现 `SYS_brk`（`a7=214`），用 arena 的空闲页扩展用户堆，并让 `demo` 分配
   一块跨页内存后写满。
