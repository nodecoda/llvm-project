# 第 02 章：内存、栈与启动

## 目标

第 01 章只把文件读进内存；本章补齐“程序开始执行前的机器状态”：

1. 32 个通用寄存器 + `pc`，`x0` 恒为零；
2. 栈区固定在 `0x7ff0_0000..0x7fff_ffff`，`sp = 0x7fff_f000`（16 字节对齐）；
3. `.bss`（`p_memsz > p_filesz` 的尾巴）必须是零，不能是文件里的残留；
4. 访问未映射地址是 **trap**（退出码 126），不是静默返回 0；
5. 内存按 4 KiB 页稀疏分配，4 GiB 地址空间不预先占满。

## 背景：QEMU 帮我们做的那部分

第一课的 `scripts/rt/start.S` **没有设置 `sp`**。在 QEMU user mode 下这不是问题：
QEMU 在跳进 `_start` 前已经把 `sp` 指到一块可写的栈上。换成自己的模拟器，
这一步没人代劳：

```text
QEMU user mode:  加载 ELF → 建栈 → 设 sp → 跳到 entry
rsim:            加载 ELF → 建栈 → 设 sp → 跳到 entry   ← 本章补最后两步
```

栈的地址不是随便选的。RISC-V 用户态惯例把栈放在地址空间顶部、向下增长：

```text
0x7ff0_0000  ┌──────────────┐  ← kStackBase
             │              │
             │   1 MiB      │
             │              │
0x7fff_f000  ├──────────────┤  ← sp（16 对齐，留 4 KiB 红区）
0x8000_0000  └──────────────┘
```

## 代码地图

| 位置 | 职责 |
| --- | --- |
| `rsim/cpu.h` | `x[32]`、`pc`、栈常量、`inStackRange` |
| `rsim/cpu.cpp` | `reset()`：加载镜像、映射栈、清零寄存器、设 `pc`/`sp` |
| `rsim/memory.cpp` | `readBytes`、页表、零初始化 |
| `rsim/main.cpp` | `--dump-mem`、reset 状态输出、trap → 126 |
| `scripts/course2-verify.sh` | `.bss` 零填充、sp 对齐、越界 trap |
| `docs/02-memory-stack.md` | 本章 |

## 关键实现

### 1. 寄存器与 reset

```cpp
for (uint32_t &reg : x)
  reg = 0;
pc = image.entry;
x[2] = kStackTop;
```

`x0` 的“恒为零”在读取时统一处理（第 03 章），写入 `x0` 会被丢弃。
`reset()` 里先清零再设 `sp`，顺序反过来会得到 `sp = 0` 这种“能跑但立刻崩”的状态。

### 2. 栈必须被映射

```cpp
memory.map(kStackBase, kStackSize);
```

`Memory::map` 按页分配，所以这条语句实际创建 256 个页。没有它，
`sp` 指向未映射地址，第一条 `sw ra, 12(sp)` 就会 trap——第 01 章的
`start.S` 正是从这条指令开始。

### 3. `.bss` 是 `memsz - filesz` 的零

`Memory::load` 先 `map(vaddr, memsz)`，再只拷贝 `filesz` 字节：

```cpp
for (uint32_t i = 0; i < seg.filesz; ++i)
  write8(seg.vaddr + i, seg.data[i]);
```

新页默认全零，所以 `.bss` 天然正确。验证脚本用一段 C：

```c
int zeroed[16];
int initialized = 0x12345678;
```

链接后 `zeroed` 落在 `p_filesz` 之外、`p_memsz` 之内；脚本从符号表取地址，
再用 `--dump-mem` 读 64 字节，必须全是 `00`。

### 4. 越界不是“读到 0”

```cpp
if (!isMapped(addr, size))
  throw MemoryError("unmapped access at " + addrString(addr));
```

`main` 捕获后打印 `rsim: trap: ...` 并返回 126。这个退出码在第 08 章会被
真正的陷阱模型取代，但现在它让“静默零内存”这种常见模拟器 bug 立刻现形。

## 构建与验证

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop

ninja -C "$B/course2"
"$W/course2/scripts/course2-verify.sh" "$B/bin" "$B/course2/rsim"
```

## 预期输出

```text
14 checks
```

手工观察：

```text
$ rsim /tmp/demo
rsim: loaded /tmp/demo: entry 0x00010000, sp 0x7ffff000,
      3 segments, 257 pages; execution arrives in chapter 03

$ rsim --dump-mem /tmp/bss 0x00010220 16
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00

$ rsim --dump-mem /tmp/bss 0x90000000 4 ; echo $?
rsim: trap: unmapped access at 0x90000000
126
```

## 常见坑

1. **`--dump-mem` 也必须先 `reset()`。** 最初版本只 `memory.load(image)`，
   于是 dump 栈地址报“unmapped”：镜像里没有栈段，栈是 CPU reset 建的。
   现在两条路径共用同一个 `Cpu::reset`。
2. **`sp` 要 16 字节对齐。** RISC-V ABI 要求调用边界上 `sp` 16 对齐；
   选 `0x7ffff000` 而不是 `0x7fffffff` 就是为此。验证脚本用
   `sp % 16 == 0` 锁住。
3. **bash 不会把 `7ffff000` 当十六进制。** `$((SP - 16))` 会报
   `value too great for base`；要写 `$((16#$SP - 16))`。
4. **`.bss` 段可能 `p_filesz = 0`。** 第 01 章的加载器已经处理；
   本章的样例故意让 `memsz > filesz`，确保零填充路径被覆盖。
5. **不要用 `std::vector<uint8_t>` 当整块 4 GiB 内存。** 稀疏页表是
   这个课程后面跑 1e6 层尾递归（栈 1 MiB）仍然轻量的原因。

## 练习

1. 把栈大小改成可配置（`--stack-size`），并让验证脚本检查新的 `sp` 仍然对齐。
2. 增加 `--dump-mem` 的 `--word` 模式，按 32 位小端打印。
3. 写一个单测：向 `x0` 写入 1 后，`x0` 仍是 0（第 03 章会用到）。
4. 把 `MemoryError` 换成带地址与访问宽度的 `Trap` 结构，为第 08 章的
   `mtval` 做准备。
