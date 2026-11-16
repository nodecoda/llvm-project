# 第 04 章：控制流与精确异常

## 目标

让三条跳转（`jal`、`jalr`、六种分支）的行为完全符合规范，并把“出错”变成
**可解释的数据**而不是一句错误字符串：

- 跳转目标必须 4 字节对齐，否则在**跳转指令**处 trap；
- `jalr` 先清 bit0，再检查对齐；
- 非对齐的 `lh/lhu/lw/sh/sw` 在**访存指令**处 trap；
- 每条 trap 都带 `(cause, tval, epc)`，`epc` 是出错指令自己的地址。

本章的 `epc` 就是第 08 章 `mepc` 的原型；本章的枚举就是 `mcause` 的原型。

## 背景：什么叫“精确”

假设一条 `lw` 在 `0x10050` 处越界：

| 做法 | 现象 |
| --- | --- |
| 不精确 | 先把 `pc` 推进到 `0x10054`，再报错；事后无法定位是哪条指令 |
| 精确 | `pc` 不动，`epc = 0x10050`，`tval = 出错地址`，`cause = 5` |

精确性是调试器和操作系统的地基：没有它，断点、单步、异常返回都做不了。
本章只做到“记录并停住”，第 08 章再接 `mtvec` 和 CSR。

另一个必须做的决定：**非对齐访存怎么办**。基础 ISA 允许“trap 或模拟”，
两条路都合法。第一课生成的代码从不需要非对齐访问，所以本章选择 **trap**，
理由是“错误必须可见”。实测对比（QEMU 6.2 用户态）：

```text
$ qemu-riscv32-static ./misaligned-load   # lw at sp+2
rvmin: ok                                  # QEMU 默默模拟了
$ rsim --run misaligned_load
rsim: trap: load address misaligned: tval=0x20000002 epc=0x00010050 (lw)
```

差异写在这里，而不是藏起来：第 05 章的差分测试只用编译器真实会生成的访问，
也就不会踩到这条分歧。哪天想对齐 QEMU，就把“trap”换成“拆成两次访问”。

## 代码地图

| 文件 | 作用 |
| --- | --- |
| `rsim/trap.h` / `trap.cpp` | `TrapCause` 枚举、`Trap(cause, tval, epc, detail)` |
| `rsim/cpu.cpp` | 目标对齐检查、访存对齐检查、六种分支 |
| `rsim/selftest.cpp` | 11 条 trap 向量（含“epc 必须是出错指令”） |
| `tests/control-flow.s` | 真实编码的分支/`jal`/`jalr` 拷问程序 |
| `tests/misaligned-jump.s` | `jalr` 跳到 `2 mod 4` |
| `tests/misaligned-load.s` | `lw` 访问 `2 mod 4` |

## 关键实现

### 1. 一个 `next`，一个出口

`step()` 只维护一个 `next`，默认 `pc + 4`；所有跳转只负责改写它，循环末尾统一
`pc = next`。这样“顺序执行”和“跳转”不需要两套收尾逻辑。

### 2. 先校验，再写 `rd`

```cpp
case Insn::JALR: {
  const uint32_t target = (a + imm) & ~1u;   // bit0 永远不参与
  if (target & 3u)                            // 2 mod 4 是非法的
    throw Trap(TrapCause::InstructionAddressMisaligned, target, at, "jalr target");
  setReg(d.rd, at + 4);                       // 校验通过才写链接值
  next = target;
  break;
}
```

顺序很重要：如果先写 `rd` 再抛异常，陷阱就变得“不精确”了——架构状态被改了一半。

### 3. 六种分支共用一条“取或不取”

`taken` 先算出来，只有 taken 时才校验目标。注意 B 型立即数的 bit0 恒为 0，
但 bit1 可以是 1，所以 `+2` 的偏移能构造出非对齐目标——`selftest` 里就有这一条。

### 4. 访存：地址算一次，错误带上下文

`load8/16/32` 与 `store8/16/32` 是 `Memory` 的唯一入口，它们把 `MemoryError`
翻译成带 `cause` 与 `epc` 的 `Trap`。`tval` 用**有效地址**（`rs1 + imm`），
不是 `rs1` 本身——这是规范定义的“出错的地址”。

## 构建与验证

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop
"$W/course2/scripts/course2-verify.sh" "$B/bin" "$B/course2/rsim"
```

第 04 章新增 3 项检查（总数 117 → 120）：

1. `tests/control-flow.s`：符号/无符号比较各两条、`INT_MIN < INT_MAX`、
   后向循环、`jal` 链接值、`jalr` 偶地址化、`ra` 往返；`main()` 必须返回 0；
2. `tests/misaligned-jump.s`：必须 `cause=0`、`tval=0x20000002`、退出码 126；
3. `tests/misaligned-load.s`：必须 `cause=4`、`tval=0x20000002`、退出码 126。

而且两个 trap 的 `epc` 都要和 `llvm-objdump -d` 里那条指令的地址**相等**——
这才是“精确”的可执行定义。

## 预期输出

```text
$ rsim --run control_flow
rsim: halted at 0x00010034 after 78 instructions (a0=0x00000001, t0=0x00000000)

$ rsim --run misaligned_jump
rsim: trap: instruction address misaligned: tval=0x20000002 epc=0x00010050 (jalr target)

$ rsim --run misaligned_load
rsim: trap: load address misaligned: tval=0x20000002 epc=0x00010050 (lw)
```

`a0=0x00000001` 是 `start.S` 传给 `write(2)` 的 fd，不是返回值；返回值在 `t0`。

## 常见坑

1. **`0x7fffffff` 不是 `lui 0x7ffff + 0x7ff`**。后者是 `0x7ffff7ff`，
   control-flow 的第 3 项因此红了一次。正确写法是 `lui 0x80000; addi -1`。
2. **`jal` 的链接值是“下一条指令”，不是“标号”**。第一版测试把 `ra` 和
   `.Lafter` 比较，但中间还夹了一条 `jal zero, .Lfail6`，于是链接值差 4。
   把 `.Lafter` 直接放在 `jal` 后面，测试才表达出真正的不变量。
3. **`awk ... { print; exit }` + `set -o pipefail` = 141**。提前退出会让
   `llvm-objdump` 收到 SIGPIPE，整个门禁以 141 收场。改成 `END { print }`
   让 awk 读完输入。
4. **对齐 trap 比 QEMU 严**。QEMU 用户态会模拟非对齐 `lw`（实测 `rvmin: ok`），
   但非对齐 `jalr` 同样 `SIGILL`（实测退出码 132）。我们选择两条都 trap，
   并在文档写清差异，避免第 05 章差分时误判。

## 练习

1. 把“非对齐 `lw`”改成模拟实现（拆两次 `lh`），让 `misaligned-load` 通过，
   然后解释为什么这会让 `epc` 的语义不再需要。
2. 给 `Trap` 增加一个 `is_interrupt` 字段，想想中断和异常在 `mepc` 上有什么不同。
3. 写一个 `jalr` 跳到 `0xFFFFFFFE` 的程序，观察 `tval` 是“清零前”还是
   “清零后”的地址，并与规范核对。
