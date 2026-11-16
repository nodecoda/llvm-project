# 第 05 章：MMIO 总线、UART 与 test finisher

## 目标

把第 04 章“在 `Machine` 里硬编码 CLINT 窗口”的做法换成一个真正的地址解码器，
并接上第一台**有外部输入输出**的设备：

- `Bus` 统一解码 RAM / CLINT / UART / test finisher，CPU 不再认识设备地址；
- UART 实现轮询控制台所需的 16550 子集：THR、RBR、IER、IIR/FCR、LCR、LSR；
- 写 THR → stdout；读 RBR → `--input <file>` 的下一个字节；
- test finisher：写 `0x5555` 成功停机，写 `0x7777` 失败，退出码放在高 16 位；
- 内核第一次真正“说话”：`hello, rmach\n`，并用 test word 收尾；
- 门禁从 8 项增加到 **11 项**，stdout 用逐字节比较，不用 `grep`。

这一章仍然只做**轮询**。UART 中断、FIFO 深度和 PLIC 留给第 09 章与“未做清单”。

## 背景：为什么设备解码必须独立

第 04 章的 `Machine::load32()` 里直接写了：

```cpp
if (clint_.contains(addr)) { ... }
```

只有一个设备时这没问题；加上 UART 和 test word 后，如果继续在 `Machine` 里堆
`if`，取指、load、store 三条路径会各长出一份地址表，迟早出现“load 能读、store
不能写”的不一致。第 05 章把这件事收进 `Bus`：

```text
CPU load/store ──> Bus::read*/write* ──┬─> test finisher (0x00100000)
                                       ├─> CLINT         (0x02000000)
                                       ├─> UART0         (0x10000000)
                                       └─> RAM           (其余地址)
```

地址与 QEMU `virt` 对齐，便于以后做可选冒烟：

| 区域 | 基址 | 本章行为 |
| --- | --- | --- |
| test finisher | `0x00100000` | 只写；低 16 位是签名，高 16 位是退出码 |
| CLINT | `0x02000000` | `mtimecmp=+0x4000`、`mtime=+0xbff8`，32 位访问 |
| UART0 | `0x10000000` | 16550 子集，**只允许字节访问** |
| DRAM | `0x80000000` | 内核镜像与栈；任意宽度 |

### 为什么先做轮询控制台

轮询版 UART 只需要两个动作：等 `LSR.THRE=1` 再写 THR；等 `LSR.DR=1` 再读 RBR。
它没有中断、没有 FIFO 时序，也没有“谁清 IIR”的问题，正好把**地址解码**和
**字节通路**单独锁住。中断版 UART 在第 09 章接入时，`Bus` 和设备接口都不用改。

### 16550 子集

| 偏移 | 读 | 写 |
| --- | --- | --- |
| `+0` | RBR：接收字节 | THR：发送字节 → stdout |
| `+1` | IER | IER |
| `+2` | IIR：本章恒为 `0x01`（无中断） | FCR：忽略 |
| `+3` | LCR | LCR |
| `+5` | LSR：`THRE=1`，有输入时 `DR=1` | — |

`LSR.THRE` 恒为 1：机器发送永不阻塞，门禁也就不需要“发送超时”分支。
`LSR.DR` 来自 `--input` 文件是否还有字节；EOF 后恒为 0。

### test finisher 的约定

SiFive 的 test device 用一个 32 位字报告结果：

| 写入值 | 含义 |
| --- | --- |
| `0x00005555` | 成功，退出码 0 |
| `0x00075555` | 成功，退出码 7（高 16 位） |
| `0x00007777` | 失败，退出码 1 |

它比 `ebreak` 更适合做门禁：内核可以在任意位置停机，不需要约定 `a0`，
而且退出码随写入值一起到达宿主。读这个地址返回 0（只写设备）。

## 代码地图

| 文件 | 作用 |
| --- | --- |
| `course3/mach/bus.{h,cpp}` | 地址解码、宽度规则、test word 状态 |
| `course3/mach/uart.{h,cpp}` | 16550 子集；THR→stdout，RBR→`--input` |
| `course3/mach/machine.{h,cpp}` | 所有访存改走 `Bus`；停机后取 `exitCode()` |
| `course3/mach/main.cpp` | `--input <file>`；版本 0.5 |
| `course3/kernel/kmain.c` | `uart_init/uart_putc/uart_puts`，输出后写 test word |
| `course3/tests/uart-echo.c` | 从 RBR 回显到第一个换行 |
| `course3/tests/finisher.s` | 写 `0x00075555`，验证高半退出码 |
| `course3/scripts/course3-verify.sh` | 11 checks |

## 关键实现

### 1. 解码顺序就是优先级

```cpp
uint32_t Bus::read32(uint32_t addr) {
  if (isTest(addr)) return 0;              // 只写设备
  if (clint_.contains(addr)) { ... }       // 32 位寄存器
  if (uart_.contains(addr)) badWidth(...); // 16550 只允许字节
  return ram_.read32(addr);                // 其余都归 RAM
}
```

三个设备窗口都先于 RAM；落不进任何窗口的地址交给 `rsim::Memory`，由它把
“未映射”变成 `MemoryError`，`Machine` 再翻译成 load/store access fault。
这样“设备地址写错”不会静默落到 RAM 里。

宽度规则是本章的契约：

- CLINT：只允许 32 位（`mtime/mtimecmp` 是 64 位，用两个 32 位字拼）；
- UART：只允许 8 位（THR/RBR/LSR 都是字节寄存器）；
- test：只允许 32 位写；
- RAM：8/16/32 位都允许。

宽度不符走 `badWidth()`，最终是 access fault，而不是把高位悄悄截掉。

### 2. UART 的“预读一个字节”

`LSR.DR` 和 `RBR` 必须看到同一个字节：如果 `LSR` 直接 `fgetc` 掉一个字节，
下一次读 RBR 就会丢数据。实现用一个 `peek_` 缓存：

```cpp
int Uart::inputByte() {
  if (peek_ == -2)                  // -2: 还没看过
    peek_ = in_ ? std::fgetc(in_) : -1;
  return peek_;
}
```

- 读 `LSR`：看 `inputByte() >= 0`，**不消费**；
- 读 `RBR`：取 `peek_`，然后重置为 `-2`，表示“下一个字节还没预读”；
- EOF 后 `peek_` 保持 `-1`，不会反复 `fgetc`。

`-2/-1` 这两个哨兵比 `bool has_peek` 少一个字段，但读代码时必须知道它们的含义；
这是本章唯一“为了少一个成员而多用一点脑力”的地方。

### 3. 输入文件的生命周期

```cpp
std::unique_ptr<std::FILE, int (*)(std::FILE *)> input(nullptr, std::fclose);
if (!opt.input.empty()) { ... input.reset(file); }

rmach::Bus bus;
bus.uart().setInput(input.get());
rmach::Machine machine;
machine.reset(image, bus);
```

顺序很关键：`Machine::reset()` 会调用 `Bus::reset()`，而 `Bus::reset()` 会调用
`Uart::reset()`。本实现的 `Uart::reset()` **只清 `peek_`，不清 `in_`**，
所以先 `setInput()` 是安全的；如果以后把 `in_ = nullptr` 加进 `reset()`，
这里就会静默丢掉输入。这个不变量写进第 4 条“常见坑”。

### 4. 停机发生在指令退休之后

```cpp
pc = next;
if (bus_->finished()) {
  halted_ = true;
  exit_code_ = bus_->exitCode();
}
```

因此写 test word 的 `sw` 是一条**正常退休**的指令：`minstret` 会计入它，
`pc` 已经指向下一条。停机的判定在 `Bus` 里完成，`Machine` 只负责取退出码；
`main` 再把退出码变成进程返回值：

```bash
$ rmach --kernel finisher.elf; echo $?
7
```

### 5. 内核侧的轮询发送

```c
static void uart_putc(char c) {
  while ((inb(UART_LSR) & LSR_THRE) == 0)
    ;
  outb(UART_THR, (unsigned char)c);
}
```

两个细节：

1. `inb/outb` 必须用 `volatile unsigned char *`，否则 `-O2` 会把轮询循环折叠；
2. 写 THR 前把 `char` 转成 `unsigned char`，避免负值符号扩展成 `0xffffffXX`。

`kmain` 先写 LCR/FCR 做最小初始化，再输出固定文本，最后写 `0x00005555`。
它不会返回，`start.S` 里的 `ebreak` 在这条路径上永远到不了。

## 构建与验证

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop

ninja -C "$B/course3"
"$W/course3/scripts/course3-verify.sh" "$B/bin" "$B/course3/rmach"
```

门禁 11 项，第 05 章新增 3 项：

1. `hello`：stdout 与 `hello, rmach\n` **逐字节**相等，退出码 0；
2. `uart-echo`：`--input` 给两行，程序只回显到第一个换行，输出与 golden 相等；
3. `finisher.s`：写 `0x00075555`，宿主退出码必须是 7。

第 01–02 章的“内核能跑”检查仍然保留，只是内核现在以 hello + test word 收尾；
CSR 的 22 条自测不受影响。

## 预期输出

```text
$ rmach --kernel hello.elf
hello, rmach
rmach: halted at 0x80000170 after 90 instructions (exit 0)

$ printf 'ping 42\nignored\n' > in.txt
$ rmach --kernel uart-echo.elf --input in.txt
ping 42
rmach: halted at 0x80000060 after 85 instructions (exit 0)

$ rmach --kernel finisher.elf; echo $?
rmach: halted at 0x80000010 after 4 instructions (exit 7)
7
```

前两行 `hello, rmach` / `ping 42` 在 stdout，`rmach: halted ...` 在 stderr；
门禁只比较 stdout，所以停机统计不会污染 golden。

指令数是当前 clang/链接器组合下的观测值，会随工具链变化；门禁只断言
stdout 字节与退出码。

## 常见坑

1. **`setInput()` 与 `reset()` 的顺序**。`Machine::reset()` 会重置 `Bus` 和 `Uart`。
   如果 `Uart::reset()` 清了 `in_`，在 `reset()` 之前 `setInput()` 就会丢输入。
   当前实现只清 `peek_`，但这是一个需要写在代码注释里的不变量。
2. **用 `grep` 检查 stdout**。`grep hello` 不会发现多一个 `\r`、少一个换行，
   或者前面混进了启动日志。golden 必须用 `cmp`/字节比较。
3. **退出码放进低半**。`0x00075555` 的高 16 位才是 7；写 `0x00005557` 会得到
   退出码 0，门禁看起来“通过”但语义全错。
4. **UART 当 32 位设备**。16550 的 THR/RBR/LSR 是字节寄存器；本章的 `Bus`
   对 16/32 位 UART 访问直接 access fault。内核 C 代码里的 `inb/outb` 必须
   是 `unsigned char` 指针。
5. **忘了 `--input` 还跑 echo**。没有输入时 `LSR.DR=0`，程序会一直轮询到
   `--max-steps`，最后以 126 退出；这不是“机器坏了”，是测试没给输入。
6. **`$B/bin/ld.lld` 不一定存在**。LLVM 构建目录只保证 `clang`/`llvm-*`；
   链接裸机 ELF 用系统 `ld.lld`（门禁里的 `LD=${LD:-ld.lld}`）。
7. **`char` 的符号**。`putc(char c)` 内部写 THR 前必须转 `unsigned char`；
   否则 0x80–0xff 的字节会被符号扩展。
8. **把停机日志写进 stdout**。`main` 的 `halted at ...` 走 stderr；如果改成
   stdout，所有 golden 都会多一行。

## 练习

1. 给 `LSR` 加 overrun（`OE`）位：在 RBR 未读走时再收到字节就置位，并写一个
   内核测试观察它。
2. 支持 `--input -` 从 stdin 读，比较文件输入与管道输入的 golden。
3. 给 test word 加 `0x7777` 失败路径的门禁，断言宿主退出码为 1。
4. 增加 `--dump-uart`，打印 IER/LCR/LSR 和已发送字节数。
5. 把 UART 的 `IER` 接上（bit 0 = 收到数据），为第 09 章的中断版控制台做准备。
6. 让 `Bus` 支持一个只读的 `--dump-bus` 视图，列出每个窗口的命中次数，
   用来解释“设备地址写错会落到哪”。
