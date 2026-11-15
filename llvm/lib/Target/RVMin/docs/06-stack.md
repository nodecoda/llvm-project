# 第 6 章：栈帧、alloca 与 load/store

## 目标

第 5 章的函数只能使用寄存器。一旦 C 代码取局部变量的地址、声明数组，或者
优化级别降到 `-O0`，就必须有栈：

```c
int dynamic(int i, int a) {
  int arr[8];
  arr[i] = a;
  return arr[i];
}

int fill(int n) {
  int a[8];
  int i;
  for (i = 0; i < n && i < 8; ++i) a[i] = i;
  int s = 0;
  for (i = 0; i < n && i < 8; ++i) s += a[i];
  return s;
}
```

完成本章后：

1. `LB/LH/LW/LBU/LHU/SB/SH/SW` 全部可用；
2. `alloca`、常量下标和变量下标都能访问；
3. 栈帧的建立/销毁（`sp` 调整、对齐）正确；
4. `storeRegToStackSlot` / `loadRegFromStackSlot` 让 `-O0` 和寄存器溢出可用；
5. callee-saved 寄存器（s0–s11）自动保存/恢复；
6. 对象偏移可以用 `FI`、`FI+常量` 两种地址形态落到一条指令里。

## 代码地图

| 文件 | 本章改动 |
| --- | --- |
| `RVMinInstrInfo.td` | 5 条 load + 3 条 store；通用 load/store 模式 |
| `RVMinISelDAGToDAG.cpp` | `FrameIndex`、`LOAD`/`STORE` 的栈地址折叠 |
| `RVMinRegisterInfo.cpp` | `eliminateFrameIndex`（FI → `sp+offset`） |
| `RVMinFrameLowering.cpp` | 序言/收尾的 `sp` 调整（含大帧拆分） |
| `RVMinInstrInfo.{h,cpp}` | `storeRegToStackSlot` / `loadRegFromStackSlot` |
| `scripts/rvmin-encoding-corpus.s` | 新增 12 条内存指令的编码门禁 |
| `llvm/test/CodeGen/RVMin/stack.ll` | 本章 FileCheck（含 `-O0`） |

## 关键实现

### 1. RV32I 的 8 条内存指令

RV32I 的 load/store 只有一种寻址方式：`base + 12 位有符号偏移`。宽度和扩展
方式由 funct3 选择：

| 指令 | funct3 | 语义 |
| --- | --- | --- |
| `LB` | `0b000` | 读 1 字节，符号扩展 |
| `LH` | `0b001` | 读 2 字节，符号扩展 |
| `LW` | `0b010` | 读 4 字节 |
| `LBU` | `0b100` | 读 1 字节，零扩展 |
| `LHU` | `0b101` | 读 2 字节，零扩展 |
| `SB` | `0b000` | 写 1 字节 |
| `SH` | `0b001` | 写 2 字节 |
| `SW` | `0b010` | 写 4 字节 |

本章只做 i32 的合法化；`i8`/`i16` 的 `extload`/`truncstore` 是第 9 章。

### 2. 栈地址在 DAG 里长什么样

`alloca` 在 SelectionDAG 里就是 `ISD::FrameIndex`。取址会变成：

```text
store volatile i32 %a, ptr %slot       → (store %a, frameindex)         + chain
gep arr, 0, 2                          → (add frameindex, 8)
%p = gep arr, 0, %i                    → (add frameindex, (shl %i, 2))
```

前两种可以折成一条 `sw a0, off(sp)`；第三种必须先把地址算进寄存器，再用
`sw a0, 0(reg)`。所以 `Select()` 里分两路：

```cpp
bool matchFrameAddr(SDValue Addr, SDValue &FI, int64_t &Offset) {
  if (isa<FrameIndexSDNode>(Addr)) { FI = Addr; Offset = 0; return true; }
  if (Addr.getOpcode() == ISD::ADD &&
      isa<ConstantSDNode>(Addr.getOperand(1)) &&
      isa<FrameIndexSDNode>(Addr.getOperand(0))) {
    FI = Addr.getOperand(0);
    Offset = cast<ConstantSDNode>(Addr.getOperand(1))->getSExtValue();
    return true;
  }
  return false;
}
```

匹配成功时直接发机器节点，`frameindex` 作为 `LW/SW` 的基址操作数保留下来：

```cpp
SDValue Ops[] = {TFI, Imm, LD->getChain()};
MachineSDNode *MN = CurDAG->getMachineNode(RVMin::LW, DL, MVT::i32,
                                           MVT::Other, Ops);
CurDAG->setNodeMemRefs(MN, {LD->getMemOperand()});
ReplaceNode(N, MN);
```

不匹配就落回 `.td` 里的一般模式：

```tablegen
def : Pat<(i32 (load GPR:$addr)), (LW GPR:$addr, 0)>;
def : Pat<(store GPR:$val, GPR:$addr), (SW GPR:$val, GPR:$addr, 0)>;
```

变量下标的 alloca 还要单独选 `ISD::FrameIndex`（它可能作为普通值出现，
例如取数组首地址）：

```cpp
case ISD::FrameIndex:
  ReplaceNode(N, getMachineNode(RVMin::ADDI, DL, MVT::i32,
                                getTargetFrameIndex(FI, MVT::i32), 0));
```

`ADDI` 的基址先留成 FI，PEI 再把它改写成 `sp`。

### 3. `eliminateFrameIndex`：FI → `sp + offset`

寄存器分配结束后，PEI 遍历所有带 FI 的指令，调用：

```cpp
bool eliminateFrameIndex(iterator II, int SPAdj, unsigned FIOperandNum,
                         RegScavenger *RS) const;
```

我们不用帧指针（`hasFP` 恒为 false），所以对象地址就是“序言执行完后的
`sp`”加上偏移。`TargetFrameLowering::getFrameIndexReference` 已经实现了这个
公式：

```text
offset = MFI.getObjectOffset(FI) + MFI.getStackSize() - LocalAreaOffset
       = (对象在帧内的偏移)      + (整个帧的大小)
```

因为 `StackGrowsDown`，`getObjectOffset` 从帧顶向下计数；加上帧大小就变成
从新 `sp` 向上计数。再把指令自带的立即数（`FI+C` 里的 C）加上：

```cpp
int64_t Offset =
    TFI->getFrameIndexReference(MF, FrameIndex, FrameReg).getFixed();
Offset += MI.getOperand(FIOperandNum + 1).getImm();

MI.getOperand(FIOperandNum).ChangeToRegister(FrameReg, false);
MI.getOperand(FIOperandNum + 1).ChangeToImmediate(Offset);
```

`FIOperandNum + 1` 之所以是立即数，是因为 load/store 的操作数布局固定为
`(值/结果, 基址, 偏移)`，而 `ADDI` 是 `(结果, 基址, 偏移)`。

偏移超出 `[-2048, 2047]` 时本章直接报错：

```text
LLVM ERROR: RVMin: frame offset 4088 does not fit in 12 bits
```

真正的大帧需要在 PEI 之后 scavenge 一个临时寄存器来物化地址；那属于优化章。

### 4. 序言与收尾

PEI 的调用顺序（`PrologEpilogInserter.cpp`）决定了指令顺序：

1. `spillCalleeSavedRegs` 先在入口块开头插入 `sw s0, ...`，在返回块插入
   `lw s0, ...`；
2. 然后 `emitPrologue` 在入口块开头插入 `addi sp, sp, -N`；
3. `emitEpilogue` 在返回块的第一条终结指令前插入 `addi sp, sp, +N`。

所以最终顺序是：

```text
prologue:  addi sp, sp, -N      ; emitPrologue 插在 spills 前面
           sw s0, off(sp)       ; PEI 的 callee-saved spill
body:      ...
epilogue:  lw s0, off(sp)       ; PEI 的 restore
           addi sp, sp, N       ; emitEpilogue 插在 restore 后面
           jalr zero, 0(ra)
```

栈对齐是 16 字节（ILP32 ABI），构造函数里：

```cpp
TargetFrameLowering(StackGrowsDown, Align(16), /*LocalAreaOffset=*/0, Align(16))
```

`LocalAreaOffset` 必须是 0；否则即使函数没有栈对象，`getFrameIndexReference`
也会认为有帧。

`ADDI` 只有 12 位立即数，所以 `|N| >= 2048` 时要拆：

```text
lui  t0, hi20
addi t0, t0, lo12
sub  sp, sp, t0        # 序言
add  sp, sp, t0        # 收尾
```

`t0` 在这个位置一定空闲：参数只可能在 a0–a7，序言里还没有活跃值，收尾里
callee-saved 已经恢复完。

### 5. 溢出与 callee-saved

`TargetInstrInfo` 的两个钩子是寄存器分配器和 PEI 共用的：

```cpp
void storeRegToStackSlot(MBB, MI, SrcReg, IsKill, FI, RC, VReg, Flags) {
  BuildMI(MBB, MI, DL, get(RVMin::SW))
      .addReg(SrcReg, getKillRegState(IsKill))
      .addFrameIndex(FI)
      .addImm(0);
}
```

注意这里**不计算偏移**：FI 原样进入指令，等 PEI 统一调用
`eliminateFrameIndex`。这也是为什么 `-O0` 能直接工作——fast regalloc 溢出
临时值时走的就是这条路径。

callee-saved 不需要自己写 `spillCalleeSavedRegisters`：`TargetFrameLowering`
的默认实现会对每个 `CalleeSavedInfo` 调用上面的 `storeRegToStackSlot`。
只要 `getCalleeSavedRegs` 里列了 s0–s11，PEI 就会自动保存被修改的那些。

`stress`（20 个同时活跃的值，`-O0`）的序言：

```text
addi sp, sp, -112
sw s0, 108(sp)
sw s1, 104(sp)
sw a0, 12(sp)
...
```

收尾按相反顺序恢复：

```text
lw s1, 104(sp)
lw s0, 108(sp)
addi sp, sp, 112
jalr zero, 0(ra)
```

## 三个真实的坑

### 坑 1：机器节点的 chain 操作数必须放在最后

第一次实现时，照着 ISD 节点的习惯写：

```cpp
SDValue Ops[] = {LD->getChain(), TFI, Imm};   // 错
```

运行直接断言：

```text
Assertion `NumMIOperands >= II.getNumOperands() ... #operands for dag node
doesn't match .td file!' failed.
```

原因：`ISD::LOAD` 的 chain 是第 0 个操作数，但 **MachineSDNode 的约定相反**——
显式操作数在前，chain 和 glue 在最后。`InstrEmitter::countOperands` 只从
尾部剥离 chain。正确写法：

```cpp
SDValue Ops[] = {TFI, Imm, LD->getChain()};
```

`STORE` 同理：`{Value, TFI, Imm, Chain}`。

### 坑 2：收尾用了 `sub` 而不是 `add`

`adjustStackPointer` 只按 `|Amount|` 物化，第一版忘了看符号，收尾也发
`sub sp, sp, t0`。2048 字节的帧立刻暴露：序言 `sp -= 2048`，收尾又
`sp -= 2048`，函数返回后栈指针永久偏移。修正：

```cpp
get(Amount > 0 ? RVMin::ADD : RVMin::SUB)
```

小帧走 `ADDI` 时因为立即数自带符号，这个 bug 看不出来——只有走到
`LUI+ADDI` 拆分路径才会现形。**测试要覆盖拆分路径**。

### 坑 3：大偏移没有 scavenging

RV32I 的 12 位偏移限制是硬约束。本章的策略是“拒绝而不是算错”：

```text
frame offset 4088 does not fit in 12 bits
```

想支持更大的帧，需要在 `eliminateFrameIndex` 里用 `RegScavenger` 借一个
临时寄存器，把 `sp+offset` 算进寄存器再访问。教学章先保持简单；第 13 章
可以把它作为优化练习。

另外两个 LLVM 24 的 API 变化值得记一笔：`TargetRegisterClass` 在本树里是
`MCRegisterClass` 的别名，没有 `hasType`，比较用
`RC == &RVMin::GPRRegClass`；`MachineInstr::setMIFlag` 已经改名为
`setFlag`（`MachineInstrBuilder::setMIFlag` 还在）。

## 门禁与验证

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop
ninja -C "$B" llc clang llvm-mc llvm-objdump llvm-readelf llvm-objcopy FileCheck

"$B/bin/llvm-lit" -sv "$W/llvm/test/CodeGen/RVMin"
# Passed: 4 (basic.ll, alu.ll, control.ll, stack.ll)

"$W/llvm/lib/Target/RVMin/scripts/check-rvmin-encodings.sh" "$B/bin"
# PASS: 83 instructions, .text identical (incl. label fixups), ELF machine RISC-V
```

`stack.ll` 的代表输出（`-O1`）：

```text
roundtrip:
	addi sp, sp, -16
	sw a0, 12(sp)
	lw a0, 12(sp)
	addi sp, sp, 16
	jalr zero, 0(ra)

array:                       # [4 x i32] 的下标 2
	addi sp, sp, -16
	sw a0, 8(sp)
	lw a0, 8(sp)
	addi sp, sp, 16

dynamic:                     # 变量下标
	slli a0, a0, 2
	addi a2, sp, 0
	add a0, a2, a0
	sw a1, 0(a0)
	lw a0, 0(a0)
```

大帧（2048 字节，`ADDI` 放不下正数）：

```text
big:
	addi sp, sp, -2048
	sw a0, 0(sp)
	lw a0, 0(sp)
	lui t0, 1
	addi t0, t0, -2048
	add sp, sp, t0
	jalr zero, 0(ra)
```

对象文件与重新汇编的 `.text` 逐字节一致：

```bash
"$B/bin/llc" -march=rvmin -mtriple=riscv32-unknown-elf -O1 stack.ll \
  -filetype=obj -o a.o
"$B/bin/llc" -march=rvmin -mtriple=riscv32-unknown-elf -O1 stack.ll -o a.s
"$B/bin/llvm-mc" -triple=rvmin -filetype=obj a.s -o b.o
# .text 380 bytes, identical
```

clang 端到端（`-O0`，局部数组 + 动态下标 + 溢出）：

```bash
"$B/bin/clang" --target=riscv32-unknown-elf -march=rv32i -mabi=ilp32 \
  -O1 -S -emit-llvm fill.c -o fill.ll
"$B/bin/llc" -march=rvmin -mtriple=riscv32-unknown-elf -O0 fill.ll -o -
```

```text
fill:
	addi sp, sp, -64
	sw a0, 20(sp)
	...
	slli a3, a0, 2
	addi a2, sp, 32       # &a[0]
	add a2, a2, a3
	sw a0, 0(a2)          # a[i] = i
	...
	lw a2, 0(a2)
	add a2, a2, a3
```

## 当前限制（第 6 章）

- 只合法化 i32 的 load/store。`char`/`short`、结构体拷贝（`memcpy`）和
  `i64` 需要第 9 章的类型合法化。
- 帧内偏移必须落在 `[-2048, 2047]`。更大的帧（超过约 2 KiB 的有效访问距离）
  会明确报错；后续用 `RegScavenger` 物化地址。
- 没有帧指针（`hasFP` 恒 false），因此也没有 `-fno-omit-frame-pointer`。
- 没有动态栈分配（VLA / `alloca` 变量大小）。
- 还没有函数调用：`ra` 仍然被保留。调用约定、栈传参、`ra` 的保存/恢复是
  第 8 章；本章的 callee-saved 机制已经就绪，届时直接复用。
- 全局变量/字符串仍在第 7 章；现在任何 `SymbolRef` 地址都会落在
  “Cannot select”。
