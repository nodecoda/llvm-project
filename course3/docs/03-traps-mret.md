# 第 03 章：精确陷阱与 mret

## 目标

把第 02 章的 CSR 从“能读能写”变成“能被硬件自动填写”：

- 同步陷阱：`ecall`（U=8 / M=11）、`ebreak`（3）、非法指令（2）、
  取指/load/store 非对齐（0/4/6）；
- 陷阱入口统一写 `mepc/mcause/mtval`，并把 `mstatus` 压栈
  （`MPIE ← MIE`、`MIE ← 0`、`MPP ← 当前特权）；
- `mret` 恢复 `M → U` 或 `U → M`，`MIE ← MPIE`；
- 两个汇编门禁分别覆盖 M 态 6 种陷阱和 U 态 3 种陷阱，并检查 `s2..s6` 未被破坏。

## 背景：trap 是数据，不是字符串

第 01 章把 `ebreak` 当成“停机”；本章开始它必须是一条**可恢复的异常**：

| 现场 | CSR | 本章的值 |
| --- | --- | --- |
| 哪条指令出错 | `mepc` | 出错指令的地址（不是下一条） |
| 为什么 | `mcause` | 0/2/3/4/6/8/11 |
| 附带信息 | `mtval` | 非法指令=指令字；ebreak=pc；访存=有效地址 |
| 返回后用什么特权 | `mstatus.MPP` | 0=U，3=M |
| 返回后开不开中断 | `mstatus.MPIE` | 旧 `MIE` |

`mtvec` 的两种模式本章都会写进实现：

- 直接模式（mode=0）：所有 trap 跳到 `base`；
- 向量模式（mode=1）：**中断**跳到 `base + 4*cause`，同步异常仍跳 `base`。
  向量模式的实际效果要到第 04 章有中断才能观察，但 CSR 写入规则现在就定下来。

## 代码地图

| 文件 | 作用 |
| --- | --- |
| `course3/mach/machine.{h,cpp}` | `enterTrap()`、`mret`、ecall/ebreak 的 cause 选择 |
| `course3/tests/traps.s` | M 态 6 连陷阱 + 期望表 |
| `course3/tests/trap-u.s` | `mret` 进入 U，再从 U 触发 3 种陷阱 |
| `course3/scripts/course3-verify.sh` | 6 checks |

## 关键实现

### 1. `step()` 把陷阱变成入口

```cpp
bool Machine::step() {
  try { return executeOne(); }
  catch (const Trap &trap) { return enterTrap(trap); }
}

bool Machine::enterTrap(const Trap &trap) {
  const uint32_t base = csr_[kCsrMtvec] & ~3u;
  if (base == 0) throw;               // 没有处理程序：交给宿主
  csr_[kCsrMepc] = trap.epc;
  csr_[kCsrMcause] = (uint32_t)trap.cause;
  csr_[kCsrMtval] = trap.tval;
  // MPIE <- MIE, MIE <- 0, MPP <- 当前特权
  ...
  priv_ = Priv::M;
  pc = base;
  return true;
}
```

`enterTrap` 直接写 `csr_[]`，不走 `writeCsr`：trap 发生时可能正处在 U 态，
而写 M CSR 的权限检查会把它自己再 trap 一次。

### 2. `mret`

```cpp
const uint32_t mpp = (mstatus >> 11) & 3;
if (mpp != 0 && mpp != 3) throw IllegalInstruction;
priv_ = (mpp == 3) ? M : U;
MIE = MPIE; MPIE = 1; MPP = U;
pc = mepc;
```

`MPP` 在返回后清零（最低特权 U），这是规范要求的“返回后不能再悄悄保持 M”。
`mret` 自己不检查 `mepc` 对齐；下一章的中断测试会覆盖这一点。

### 3. `ecall` 的 cause 取决于特权

```cpp
throw Trap(priv_ == U ? EcallFromU : EcallFromM, 0, at, "ecall");
```

`ebreak` 的 `mtval` 是**它自己的地址**，不是 0——处理程序用这个值区分
“断点在第几条指令”。

### 4. 没有 handler 时的引导约定

```cpp
if ((csr_[kCsrMtvec] & ~3u) == 0) {   // 还没装 handler
  halted_ = true;
  exit_code_ = (int)reg(10);          // a0
}
```

这让第 01/02 章的内核仍然能停机，也让本章测试在最后用
`csrw mtvec, zero; ebreak` 干净退出。装上 `mtvec` 之后，`ebreak` 就是
真正的 Breakpoint trap。

## 构建与验证

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop

ninja -C "$B/course3"
"$W/course3/scripts/course3-verify.sh" "$B/bin" "$B/course3/rmach"
```

门禁 6 项（`6 checks`），第 03 章新增 2 项：

1. `tests/traps.s`：M 态的 6 种陷阱，处理程序逐项比较
   `(mcause, mepc, mtval)`，并检查 `s2..s5` 未变；
2. `tests/trap-u.s`：`mret` 进 U，`ecall/ebreak/非法指令` 从 U 触发，
   再 `mret` 回 M 停机。

失败时退出码是 `10*trap_index + stage`：`stage` 1/2/3 分别表示
cause/epc/tval 不匹配，方便定位（例如 41 = 第 5 个陷阱的 cause 错）。

## 预期输出

```text
$ rmach --kernel traps.elf
rmach: halted at 0x80000074 after 218 instructions (exit 0)

$ rmach --kernel trap-u.elf
rmach: halted at 0x8000005c after 92 instructions (exit 0)
```

## 常见坑

1. **处理程序破坏了被中断代码的临时寄存器**。`traps.s` 第一次失败是
   `sw` 用了 `t0`，而 handler 把 `t0` 改成 `mcause`；跨陷阱的状态必须放在
   `s` 寄存器或内存里。
2. **期望表里的标签放在设置指令之前**。`la/li` 是两条指令，标签必须紧贴
   真正会陷阱的那一条，否则 `mepc` 与期望值差 4 或 8。
3. **`ecall` 的 `mtval` 写成 pc**。规范里 `ecall` 的 `mtval=0`，
   `ebreak` 才是 pc；两者混用会让处理程序无法区分。
4. **`enterTrap` 调用 `writeCsr`**。U 态的 trap 会被权限检查二次 trap，
   表现为“嵌套陷阱”或死循环；入口必须直接写 `csr_[]`。
5. **以为 `mret` 会检查 `mepc` 对齐**。它不会；非对齐的 `mepc` 要到
   下一条取指才触发 `InstructionAddressMisaligned`。
6. **忘了 `MPP` 清零**。`mret` 之后 `MPP` 应回到 U，否则下一次 trap 的
   `MPP` 会记错，返回路径就错了。

## 练习

1. 把 `enterTrap` 拆成 `saveContext()` / `jumpToHandler()`，为第 08 章的
   上下文切换做准备。
2. 增加 `mcause` 的优先级测试：同一条指令既非法又非对齐时，谁先报？
3. 实现 `mtvec` 向量模式的中断入口（第 04 章会用到）。
4. 给 `trap-u.s` 增加一次 `mret` 回 U 后继续执行，验证 `MPIE/MIE` 的语义。
5. 用 `--trace` 统计两个测试的指令数，解释 handler 的额外开销。
