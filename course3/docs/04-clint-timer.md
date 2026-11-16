# 第 04 章：CLINT 与定时器中断

## 目标

让第一台设备上线，并让“中断”第一次真的发生：

- CLINT：`mtime`（64 位、只读、每退休一条指令 +1）与 `mtimecmp`（64 位、可写）；
- `mtime >= mtimecmp` 置 `mip.MTIP`；
- `mstatus.MIE && mie.MTIE && mip.MTIP` 时，在**取下一条指令之前**进入中断；
- `mcause = 0x80000007`，`mtval = 0`，`mepc = 被打断指令的地址`；
- 支持 `mtvec` 向量模式：中断跳到 `base + 4*7`，同步异常仍跳 `base`；
- 反向测试：中断使能但 `mtimecmp` 永远够不到时，不得有伪中断。

## 背景：为什么定时器是第一个设备

它不需要外部输入，也不依赖 UART 或磁盘；只要“时间会走”，中断就能被观察到。
第三课的周期模型沿用第二课第 10 章：**一条退休指令 = 一个周期**。

CLINT 的地址对齐 QEMU `virt`：

| 寄存器 | 地址 | 宽度 | 属性 |
| --- | --- | --- | --- |
| `mtimecmp` | `0x02004000` | 64 位（低/高两个字） | 可读写 |
| `mtime` | `0x0200bff8` | 64 位（低/高两个字） | 只读 |

RV32 没有 64 位 load/store，所以软件要分两次访问；这也是本章唯一需要
“两个字拼一个值”的地方。

## 代码地图

| 文件 | 作用 |
| --- | --- |
| `course3/mach/clint.{h,cpp}` | `mtime/mtimecmp`、`tick()`、`pending()` |
| `course3/mach/machine.cpp` | MMIO 拦截、`updateTimer()`、`takeInterrupt()` |
| `course3/tests/timer.s` | 3 次向量模式中断 + 单调性检查 |
| `course3/tests/timer-off.s` | 使能中断但永不触发 |
| `course3/scripts/course3-verify.sh` | 8 checks |

## 关键实现

### 1. 每退休一条指令，时间 +1

```cpp
++executed_;
clint_.tick();
updateTimer();      // mip.MTIP = mtime >= mtimecmp
```

`tick()` 放在 `++executed_` 之后：被执行但触发陷阱的指令也会让时间前进，
这与 `minstret` 的口径一致。

### 2. MMIO 拦截放在访存助手里

```cpp
uint32_t Machine::load32(uint32_t addr, uint32_t epc) {
  if (clint_.contains(addr)) {
    if (addr == Clint::kMtime)     return (uint32_t)clint_.mtime();
    if (addr == Clint::kMtime + 4) return (uint32_t)(clint_.mtime() >> 32);
    ...
  }
  ...
}
```

第 05 章会把这里换成总线；现在先让 CLINT 顶替两个地址窗口。
8/16 位访问 CLINT 一律 `Load/StoreAccessFault`——设备寄存器是 32 位宽的。

### 3. 中断在取指之前

```cpp
bool Machine::step() {
  if (takeInterrupt()) return true;   // 这条指令没有退休
  try { return executeOne(); } catch (const Trap &t) { return enterTrap(t); }
}
```

因此 `mepc` 是**还没执行的那条指令**；`mret` 回去会重新执行它。
`takeInterrupt()` 只有在 `mtvec != 0` 时才生效，保持第 01 章的引导行为。

### 4. 向量模式

```cpp
if ((cause & kInterruptBit) && (mtvec & 1))
  base += 4 * (cause & 0x7fffffff);
```

测试里的 `vector` 表有 8 个 `.word`，第 8 个（索引 7）才是 timer handler。
如果实现忽略了 mode 位，`pc` 会落到索引 0 的 0 字上，立刻非法指令——门禁会红。

### 5. 处理程序必须重新 arm

```asm
lw   t4, 0(t3)      # mtimecmp low
addi t4, t4, 200
sw   t4, 0(t3)
```

不推进 `mtimecmp`，`MTIP` 永远为 1，`mret` 后立刻再次中断，形成中断风暴。

## 构建与验证

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop

ninja -C "$B/course3"
"$W/course3/scripts/course3-verify.sh" "$B/bin" "$B/course3/rmach"
```

门禁 8 项，第 04 章新增 2 项：

1. `tests/timer.s`：3 次中断，校验 `mcause/mtval/mepc`、`mtime` 单调、重新 arm；
2. `tests/timer-off.s`：`mtimecmp = 0xffffffffffffffff`，使能 MIE+MTIE 后
   跑 1000 次循环，不得触发。

## 预期输出

```text
$ rmach --kernel timer.elf
rmach: halted at 0x800000e0 after 634 instructions (exit 0)

$ rmach --kernel timer-off.elf
rmach: halted at 0x8000004c after 2017 instructions (exit 0)
```

`timer.s` 的 634 条 = 启动 + 约 200 条等待 + 3 次 handler 的固定开销；
具体数字随汇编器版本变化，门禁只断言退出码。

## 常见坑

1. **把 MIE 当成 bit 7**。`mie.MTIE` 是 bit 7（`0x80`），
   `mstatus.MIE` 是 bit 3（`0x8`）。第一次实现两者都写 `0x80`，
   结果中断永远不触发，测试直接撞 1000 万步上限。
2. **中断入口写成 `pc + 4`**。中断发生在取指之前，`mepc` 必须是
   **当前** `pc`；写成 `pc+4` 会跳过一条指令。
3. **`mtimecmp` 只写低 32 位**。高字保留旧值（通常是 0），对 64 位比较没问题；
   但如果高字残留 `0xffffffff`，低字永远追不上。
4. **忘了 `updateTimer()`**。只改 `mtimecmp` 不重算 `MTIP`，中断会延迟到
   下一次 `tick()` 才消失/出现，表现为多一次或少一次。
5. **把向量模式用在同步异常上**。只有中断走 `base + 4*code`；
   非法指令仍然跳 `base`。
6. **`mtime` 可写**。它由机器递增；`sw` 到 `mtime` 必须是 `StoreAccessFault`。

## 练习

1. 给 CLINT 加 `mtimecmp` 的高字写保护，验证 64 位回绕。
2. 实现 `mip.MSIP`/`mie.MSIE`（软件中断），用 `mip` 写 1 触发。
3. 把周期模型改成“每条指令 N 个周期”，观察 tick 数变化。
4. 增加 `--dump-clint`，打印 `mtime/mtimecmp/MTIP`。
5. 在 `timer.s` 里把 handler 放到直接模式，验证两种模式的差异。
