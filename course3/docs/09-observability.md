# 第 09 章：调试与可观测性

## 目标

让 `rmach` 在“能跑”之后变得“能查”：

- `--trace` 每退休一条指令打印一行，包含 `pc`、取指物理地址、特权级、
  指令字和反汇编；
- `--stats` 汇总指令数、定时器中断次数、`ecall` 次数；
- `--dump-csr` 打印全部 15 个已实现 CSR；
- `--dump-procs` 打印每个进程的状态、退出码、`pc` 和 `satp`；
- `--dump-pagetable pid<N>` 按进程选择根表，而不是只能看最后一个 `satp`；
- 门禁从 21 项增加到 **24 项**，并且第一次把三个独立计数器对齐：
  trace 行数 == `--stats` 的 `instructions` == `minstret`。

本章不新增指令或设备；它只把已有状态暴露出来，并为第 10 章的验收提供证据。

## 背景：为什么是这三个数

“机器执行了多少条指令”有三个来源：

| 来源 | 含义 | 可能出错的地方 |
| --- | --- | --- |
| `--trace` 行数 | 每退休一条指令写一行 | 在错误的位置打印/跳过、缓冲丢失 |
| `--stats` 的 `instructions` | `Machine::executed_` | 在陷阱路径重复计数 |
| `minstret` | CSR `0xb02` | 读的是旧值，或被特权检查挡住 |

三者相等是一个很便宜、但很强的自洽检查：任何一条指令路径如果漏掉 trace、
重复计数，或者 `minstret` 没有跟着 `executed_` 走，门禁都会失败。第 02 章
已经把 `minstret` 实现成 `executed_` 的只读视图，本章只需要把它打印出来。

### 为什么 trace 要带 PA

U 态的 `pc` 是虚拟地址，`mepc/mtval` 也是虚拟地址；只有总线和页表看到物理地址。
调试时最常见的两个问题：

1. “页表看起来对，但取指 PA 不对”；
2. “同一个 VA 在两个进程里指向不同 PA”。

trace 行里的第二个字段就是**当前取指的物理地址**。它由 `fetchWord()` 在翻译后
记录，和真正送到总线的地址是同一个值，不是重新算一遍。

### 为什么还要按 pid dump 页表

第 08 章之后，`satp` 在停机时只保留**最后一个运行过**的进程的根表。`pid0` 的
页表还在内存里，但 `--dump-pagetable` 默认看不到。把每个进程的 `satp` 发布到
`boot_result`，再让 `--dump-pagetable pid0` 从结果页取根表，就能对照两个进程的
地址空间。

## 代码地图

| 文件 | 作用 |
| --- | --- |
| `course3/mach/machine.{h,cpp}` | `last_fetch_pa_`、trace 格式、计数器 |
| `course3/mach/main.cpp` | `--dump-csr`、`--dump-pagetable pid<N>`、`--stats` |
| `course3/kernel/boot.h` | `boot_proc_result` 增加 `satp` |
| `course3/kernel/proc.c` | `proc_publish()` 发布 `satp` |
| `course3/scripts/course3-verify.sh` | 24 checks |

## 关键实现

### 1. trace 格式

第 08 章之前：

```text
T 80000000 00000297 auipc t0, 0x0
```

第 09 章：

```text
T 80000000 80000000 M 00000297 auipc t0, 0x0
T 00010000 80400000 U ff010113 addi sp, sp, -16
```

字段依次是：

| 列 | 含义 |
| --- | --- |
| 1 | `T`，固定前缀 |
| 2 | `pc`（虚拟地址） |
| 3 | 取指物理地址 |
| 4 | `M` 或 `U`，执行这条指令时的特权级 |
| 5 | 32 位指令字 |
| 6+ | 反汇编 |

`pc` 仍是第 2 列，所以第 01 章“第一条 trace 的地址是 `0x80000000`”的门禁
不用改。

实现只有三处：

```cpp
// fetchWord()
last_fetch_pa_ = pa;

// executeOne()，在 decode 之后、执行之前
if (trace_) {
  const char priv = priv_ == Priv::M ? 'M' : 'U';
  std::fprintf(trace_, "T %08x %08x %c %08x %s\n", at, last_fetch_pa_, priv,
               word, formatForTrace(d).c_str());
}
```

`priv` 必须在执行前捕获：`mret` 会把 `priv_` 从 M 改成 U，如果先执行再打印，
`mret` 这一行会被标成 `U`，`ecall` 这一行也会错。

### 2. `last_fetch_pa_` 的生命周期

`fetchWord()` 每次取指都会重写它，包括：

- M 态内核取指（恒等映射，PA == VA）；
- U 态用户取指（Sv32 翻译，PA 来自页表）；
- 页错误前的取指（`translate()` 抛异常时还没有写成功，trace 不会打印）。

`Machine::reset()` 把它清零，避免第一次调试时看到上一个镜像的残留值。

### 3. `--dump-csr`

15 个 CSR 的名字和编号集中在一张表里：

```cpp
const struct { const char *name; uint16_t number; } names[] = {
  {"mstatus", kCsrMstatus}, {"misa", kCsrMisa}, ...
  {"minstret", kCsrMinstret}, {"satp", kCsrSatp},
};
```

输出：

```text
csr mstatus=0x00000080
csr misa=0x40001100
csr mie=0x00000080
csr mtvec=0x80000d0c
csr mscratch=0x7feffff0
csr mepc=0x00010040
csr mcause=0x00000008
csr mtval=0x00000000
csr mip=0x00000000
csr mhartid=0x00000000
csr mvendorid=0x00000000
csr marchid=0x00000000
csr mimpid=0x00000000
csr minstret=0x00004947
csr satp=0x80080001
```

`--dump-csr` 在停机之后调用 `Machine::readCsr()`。停机一定发生在 M 态
（`finish()` 只由内核的陷阱处理调用），所以特权检查不会挡住这些 CSR。

### 4. 按 pid dump 页表

`boot_proc_result` 增加一个字段：

```c
struct boot_proc_result {
  uint32_t pid;
  uint32_t state;
  uint32_t exit_code;
  uint32_t pc;
  uint32_t satp;   // 本章新增
};
```

`--dump-procs` 的输出也多了 `satp`：

```text
proc 0 state=EXITED exit=7 pc=0x00010020 satp=0x80080001
proc 1 state=EXITED exit=0 pc=0x00010020 satp=0x80080002
```

`--dump-pagetable` 的参数现在有两种形式：

```bash
rmach ... --dump-pagetable            # 停机时的 satp
rmach ... --dump-pagetable 0x80080001 # 显式 satp
rmach ... --dump-pagetable pid0       # 从 boot_result 取 pid 0 的 satp
```

`pid<N>` 走结果页；如果结果页 magic 不对、`pid >= nproc` 或进程不存在，
`rmach` 打印 `no page table for pid N` 并以 2 退出。

### 5. `--stats` 的三个计数器

```text
rmach: stats: instructions=18759 timer_ticks=1 ecalls=2
```

- `instructions`：`Machine::executed_`，每退休一条指令加一；
- `timer_ticks`：`enterTrap()` 里 cause 为 `0x80000007` 时加一；
- `ecalls`：`enterTrap()` 里 cause 为 8 时加一（来自 U 态；M 态的 `ecall`
  是 cause 11，不计入）。

这三个数都在 stderr，stdout 仍然只属于用户程序。

## 构建与验证

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop

ninja -C "$B/course3"
"$W/course3/scripts/course3-verify.sh" "$B/bin" "$B/course3/rmach"
```

门禁 24 项，第 09 章新增 3 项：

1. **三个计数器对齐**：`--trace` 的 `T ` 行数 == `--stats` 的
   `instructions` == `--dump-csr` 的 `minstret`；
2. **CSR 值**：`misa=0x40001100`、`mhartid=0`、`satp` 的 bit 31 为 1；
3. **U 态翻译**：trace 里存在
   `T 00010000 80400000 U ...`，即 demo 的第一条指令从 VA `0x10000`
   取到 PA `0x80400000`。

第 08 章的 21 项继续通过；第 01 章的 trace 首行检查只读第 2 列，因此不受
格式扩展影响。

## 预期输出

```text
$ rmach --version
rmach 0.9 (chapter 09)

$ rmach --kernel kernel.elf --user demo --trace --stats --dump-csr
rvmin: ok
csr mstatus=0x00000080
csr misa=0x40001100
...
csr minstret=0x00004947
csr satp=0x80080001
rmach: stats: instructions=18759 timer_ticks=1 ecalls=2
rmach: halted at 0x80000804 after 18759 instructions (exit 0)
```

stderr 同时包含 18759 行 trace；`0x4947 == 18759`，三者一致。

```text
$ rmach --kernel kernel.elf --user demo --dump-pagetable pid0
rvmin: ok
satp 0x80080001 mode=1 asid=0x000 ppn=0x080001
leaf va=0x00010000 pa=0x80400000 pte=0x201000df level=0 flags=VRWXUAD
leaf va=0x10000000 pa=0x10000000 pte=0x040000d7 level=0 flags=VRWUAD
...
```

具体指令数会随编译器/链接布局变化；门禁只断言三个计数器相等，以及 U 态
首条 trace 的 VA/PA。

## 常见坑

1. **trace 在 `mret` 之后才捕获特权级**。`mret` 执行前是 M、执行后是 U；如果
   先执行再打印，这一行会被标成 `U`。必须在 `executeOne()` 开头捕获 `priv`。
2. **用 `pc` 重新翻译一次来填 PA**。这会多走一次页表，还可能因为 `A/D` 或
   TLB 状态得到不同结果。直接从 `fetchWord()` 记录实际使用的 PA。
3. **把 trace 写到 stdout**。用户程序的 golden 会被 trace 行污染；trace、stats、
   停机统计都走 stderr。
4. **`--dump-csr` 在 U 态调用 `readCsr()`**。`readCsr()` 对 U 态访问 M CSR
   会抛非法指令；当前停机点一定在 M 态，但不要把 dump 移到运行中的 U 态路径。
5. **`boot_proc_result` 加字段后忘了同步偏移**。`--dump-procs` 原来按 16 字节
   一条读，增加 `satp` 后是 20 字节；不同步会打印出 `pc=0x00000000
   satp=0x...` 这类错位值。
6. **`pid` 和 `satp` 参数混淆**。`pid0` 是进程号，`0x80080001` 是 satp；
   `--dump-pagetable 0` 会按 satp=0 处理并打印 `bare: no page table`。
7. **`minstret` 按十进制解析**。`--dump-csr` 打印的是 `0x...`；门禁必须用
   `16#` 或 `strtoul(..., 16)` 转换。
8. **trace 文件太大**。两进程 15 万条指令会产生 15 万行；本章的 trace 门禁用
   demo（约 1.8 万行），不要用长跑程序做格式检查。
9. **`--stats` 在 `--trace` 之前统计**。`--stats` 读的是停机后的
   `executed_`；如果中途打印，数字会少一截。统一放在停机之后。
10. **`proc_publish()` 没有写 `satp`**。`--dump-pagetable pid0` 会取到 0，
    打印 `bare`，看起来像页表没建；实际上是结果页字段没填。

## 练习

1. 在 trace 行里加入 load/store 的 `va->pa`：记录最后一条访存的地址和类型，
   并保持“一条指令一行”。
2. 给 `--stats` 增加每个进程的指令数、trap 数和页错误数。
3. 让 `--dump-pagetable` 支持 `--va 0x10000`，只打印命中该 VA 的路径。
4. 增加 `--dump-mem <addr> <len>`，按 UNMAPPED/RAM/MMIO 标注每一页。
5. 把 trace 写成 CSV（`pc,pa,priv,word`），用 `awk` 统计每个 VA 的取指次数。
6. 实现 `--trace-filter <pid|M|U>`，只打印匹配的行，并验证行数与过滤条件一致。
7. 给 `--dump-procs` 增加每进程的 `steps`，与 `--stats` 的总和对照。
8. 在页表 dump 里标出每个叶子的 `A/D` 是否为软件预置，和第 06 章的手算表对照。
