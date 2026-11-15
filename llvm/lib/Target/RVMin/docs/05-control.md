# 第 5 章：控制流、条件分支与基本块布局

## 目标

第 4 章的函数都只有一个基本块，结尾一条 `ret`。本章让后端理解“块”：

```c
int max(int a, int b) { return a > b ? a : b; }

int count_down(int n) {
  int acc = 0;
  while (n > 0) { acc += n; --n; }
  return acc;
}
```

完成本章后：

1. 6 条条件分支 `beq/bne/blt/bge/bltu/bgeu` 都能选择；
2. 无条件跳转 `j label`（RV32I 里就是 `jal x0, label`）能选择；
3. `br bb`、`br i1 %c, label %t, label %f` 都能出汇编；
4. 分支目标是基本块符号，由汇编器后端的 fixup 回填成 B/J 型立即数；
5. `MachineBranchFolder` / `MachineBlockPlacement` 能工作：跳到布局后继的分支被删掉，
   循环的回边变成 fallthrough。

## 代码地图

| 文件 | 本章改动 |
| --- | --- |
| `RVMinInstrInfo.td` | `BLTU/BGEU`；`RVMinISD::BR_CC`；分支 `Pat`；`PseudoBR` |
| `RVMinISelLowering.{h,cpp}` | `BRCOND`/`BR_CC` → `Custom`；条件码规范化 |
| `RVMinMCInstLower.{h,cpp}` | `MO_MachineBasicBlock` → `MCSymbolRefExpr` |
| `RVMinAsmPrinter.cpp` | 传 `MCContext`；为伪指令展开提供 `lowerOperand` |
| `MCTargetDesc/RVMinMCCodeEmitter.cpp` | `BLTU/BGEU` 走 `fixup_rvmin_branch` |
| `MCTargetDesc/RVMinAsmBackend.cpp` | 修掉 `encodeJalImm` 的真实 bug |
| `RVMinInstrInfo.{h,cpp}` | `analyzeBranch` / `removeBranch` / `insertBranch` |
| `scripts/rvmin-fixup-corpus.s` | 新门禁：label 目标的前向/后向 fixup |
| `llvm/test/CodeGen/RVMin/control.ll` | `max`/`count_down`/`nested` 的 FileCheck |

## 关键实现

### 1. `BRCOND` 只有布尔值，`BR_CC` 才携带比较

LegalizeDAG 把 `br i1 %c` 变成 `ISD::BRCOND`，它的操作数只有
`(chain, condition, dest)`——**没有 lhs/rhs**。如果只降级 `BRCOND`，就只能先
`setcc ne cond, 0` 再比较，白白多一条指令。

LLVM 为此提供了 `ISD::BR_CC`：`(chain, cc, lhs, rhs, dest)`。本章定义自己的
目标节点，让 TableGen 直接用比较操作数匹配分支指令：

```tablegen
def SDT_RVMinBrCC : SDTypeProfile<0, 4, [SDTCisSameAs<0, 1>,
                                         SDTCisVT<2, OtherVT>,
                                         SDTCisVT<3, OtherVT>]>;
def RVMinBrcc : SDNode<"RVMinISD::BR_CC", SDT_RVMinBrCC, [SDNPHasChain]>;
```

`SDNPHasChain` 必须有；`SDTCisVT<2/3, OtherVT>` 表示第 3、4 个操作数是
`condcode` 和基本块，而不是 i32。

降级分两步，都在 `RVMinISelLowering.cpp`：

```cpp
case ISD::BRCOND: return lowerBRCOND(Op, DAG);
case ISD::BR_CC:  return lowerBR_CC(Op, DAG);
```

- `lowerBRCOND`：条件来自 `SETCC` 时，把 `SETCC` 的 lhs/rhs 直接搬进 `BR_CC`
  （融合）；否则退化成 `cond != 0`。
- `lowerBR_CC`：Generic DAG 自己产生的 `BR_CC`（例如 `and i1` 之后的比较），
  同样规范化后转成 `RVMinISD::BR_CC`。

这样 `max` 的 DAG 末端是：

```text
RVMinISD::BR_CC  chain, SETGT, %a, %b, %else
```

而不是一条 `setcc` + 一条只认布尔的分支。

### 2. 六条分支 + 条件码规范化

RV32I 只有 6 条条件分支，而 `ISD::CondCode` 有 10 种整数比较。与其为
`SETGT/SETLE/SETUGT/SETULE` 各生成一条 `xori`，不如在 C++ 里换操作数：

| 原始条件 | 规范化后 | 做法 |
| --- | --- | --- |
| `a > b` | `b < a` | 交换 lhs/rhs |
| `a <= b` | `b >= a` | 交换 lhs/rhs |
| `a >u b` | `b <u a` | 交换 lhs/rhs |
| `a <=u b` | `b >=u a` | 交换 lhs/rhs |
| `eq/ne/lt/ge/ltu/geu` | 不变 | — |

```cpp
case ISD::SETGT:  CC = ISD::SETLT;  std::swap(LHS, RHS); return;
```

规范化之后，TableGen 只需要六个模式，每个模式再分“寄存器-寄存器”和
“寄存器-零”两种：

```tablegen
multiclass RVMinBccPat<CondCode Cond, RVMinBInst Inst> {
  def : Pat<(RVMinBrcc GPR:$rs1, GPR:$rs2, Cond, bb:$dst),
            (Inst GPR:$rs1, GPR:$rs2, simm13_lsb0:$dst)>;
  // a == 0 / a >= 0 之类直接用 x0，不物化常量 0
  def : Pat<(RVMinBrcc GPR:$rs1, 0, Cond, bb:$dst),
            (Inst GPR:$rs1, X0, simm13_lsb0:$dst)>;
}
defm : RVMinBccPat<SETEQ, BEQ>;
defm : RVMinBccPat<SETNE, BNE>;
defm : RVMinBccPat<SETLT, BLT>;
defm : RVMinBccPat<SETGE, BGE>;
defm : RVMinBccPat<SETULT, BLTU>;
defm : RVMinBccPat<SETUGE, BGEU>;
```

第 1 章的 `.td` 里只有 `BEQ/BNE/BLT/BGE`；本章补上
`BLTU`（funct3 `0b110`）和 `BGEU`（`0b111`），无符号比较才完整。

### 3. `bb:$dst` 要求操作数是 `Operand<OtherVT>`

第 1 章把分支偏移定义成 `Operand<i32>`：

```tablegen
def simm13_lsb0 : Operand<i32> { ... }
```

这样只能匹配立即数。`bb:$dst` 的类型是 `OtherVT`（基本块符号），TableGen 在
模式类型检查时直接失败。改成：

```tablegen
def simm13_lsb0 : Operand<OtherVT> {
  let PrintMethod = "printOperand";
  let ParserMatchClass = SImm13Lsb0AsmOperand;
  let MCOperandPredicate = [{
    int64_t Imm;
    if (MCOp.evaluateAsConstantImm(Imm))
      return isShiftedInt<12, 1>(Imm);
    return MCOp.isBareSymbolRef();   // 标签目标
  }];
}
```

`MCOperandPredicate` 也一起改了：以前只接受常量立即数，现在还要接受
汇编器里的裸符号引用（`beq a0, a1, .LBB0_2`）。只改类型不改谓词的话，
`llvm-mc` 会拒绝所有带标签的分支。

`printOperand` 不用变：`MCInst` 里操作数已经是 `MCSymbolRefExpr`，打印时
自然输出标签名。

### 4. 无条件跳转：`PseudoBR` + `PseudoInstExpansion`

RV32I 的 `j label` 就是 `jal x0, label`。能不能直接写：

```tablegen
def : Pat<(br bb:$dst), (JAL X0, simm21_lsb0:$dst)>;
```

不行。`JAL` 的 `OutOperandList` 带着 `GPR:$rd`，TableGen 在模式里看到两个
操作数会报：

```text
error: Instruction 'JAL' was provided 2 operands but expected only 1
```

改成“无结果的伪指令 + 展开到 JAL”：

```tablegen
def PseudoBR : RVMinPseudo<(outs), (ins simm21_lsb0:$imm21), "", []>,
               PseudoInstExpansion<(JAL X0, simm21_lsb0:$imm21)> {
  let isBranch = 1;
  let isBarrier = 1;
  let isTerminator = 1;
  let Size = 4;
}
def : Pat<(br bb:$dst), (PseudoBR simm21_lsb0:$dst)>;
```

这样做的好处：

- 伪指令没有 `rd`，寄存器分配器不会去分配一个恒为 `x0` 的虚拟寄存器；
- 打印汇编时才展开成 `jal zero, .LBB1_1`，编码仍然走 `JAL` 的表；
- `Size = 4` 让分支分析能算对字节数（`PseudoBR` 自身 `isPseudo`）。

`PseudoInstExpansion` 会生成 `RVMinGenMCPseudoLowering.inc`，里面调用
AsmPrinter 的：

```cpp
void lowerOperand(const MachineOperand &MO, MCOperand &MCOp);
```

所以 `RVMinAsmPrinter` 必须提供这个方法，并且处理 `MO_MachineBasicBlock`：

```cpp
case MachineOperand::MO_MachineBasicBlock:
  MCOp = MCOperand::createExpr(
      MCSymbolRefExpr::create(MO.getMBB()->getSymbol(), OutContext));
  return;
```

### 5. 分支目标进入 MC 层：符号 + fixup

普通指令的 lowering 在 `RVMinMCInstLower.cpp`。本章加一种机器操作数：

```cpp
case MachineOperand::MO_MachineBasicBlock:
  OutMI.addOperand(MCOperand::createExpr(
      MCSymbolRefExpr::create(MO.getMBB()->getSymbol(), Ctx)));
  break;
```

因此 `LowerRVMinMachineInstrToMCInst` 需要多一个 `MCContext &` 参数
（`MCSymbolRefExpr::create` 要用它）。`RVMinAsmPrinter` 传 `OutContext`。

到了 `RVMinMCCodeEmitter`，分支和跳转的操作数不再是立即数，而是表达式，
所以走 fixup：

```cpp
bool IsPCRel = Kind == rvmin::fixup_rvmin_branch ||
               Kind == rvmin::fixup_rvmin_jal;
Fixups.push_back(MCFixup::create(0, MO.getExpr(), (MCFixupKind)Kind, IsPCRel));
```

`MCAssembler` 解析完布局后算出 `Value = 目标地址 - 本条指令地址`，回调
`RVMinAsmBackend::applyFixup`，由 `encodeBranchImm` / `encodeJalImm`
把字节偏移塞进 B/J 型字段：

```text
B: imm[12] -> bit31, imm[10:5] -> 30:25, imm[4:1] -> 11:8, imm[11] -> bit7
J: imm[20] -> bit31, imm[10:1] -> 30:21, imm[11] -> bit20, imm[19:12] -> 19:12
```

偏移不是 2 字节对齐、超出 ±4 KiB（B）或 ±1 MiB（J）时，`applyFixup`
通过 `getContext().reportError` 报错，而不是悄悄截断。

### 6. 基本块分析：`analyzeBranch` / `removeBranch` / `insertBranch`

只有分支指令还不够。`BranchFolder`、`MachineBlockPlacement`、`TailDuplication`
都通过 `TargetInstrInfo::analyzeBranch` 理解块尾：

```cpp
bool analyzeBranch(MachineBasicBlock &MBB, MachineBasicBlock *&TBB,
                   MachineBasicBlock *&FBB,
                   SmallVectorImpl<MachineOperand> &Cond,
                   bool AllowModify) const override;
```

契约是：从块尾往前跳过 debug/CFI 指令，只接受

- 什么都没有（fallthrough）；
- 一条无条件分支（`PseudoBR`，或展开后的 `jal zero, x`）；
- 一条或两条条件分支；两条时必须是**同一个**条件（块合并会产生这种形状）。

返回值 `true` 表示“看不懂”。我们的 `Cond` 布局是固定的三元组：

```text
Cond = [opcode, rs1, rs2]
```

`insertBranch` 靠它重建指令，`assert(Cond.size() == 3 || Cond.empty())`。
`FBB` 是条件分支的 fallthrough 目标（`cond br X; jmp Y` 时 `TBB=X, FBB=Y`）。

`AllowModify = true` 时要做三件事：

1. 删掉分支之后的不可达指令；
2. 如果无条件分支的目标就是布局后继（`MBB.isLayoutSuccessor(Dest)`），
   删掉这条分支，让控制流自然落下；
3. 其余情况保持原样。

第 2 条就是 `max` 里看不到 `j` 的原因：`then` 块 `ret`，`else` 块紧挨着，
`jal` 到布局后继被删了。循环里则相反——`count_down` 的回边不是布局后继，
所以保留 `jal zero, .LBB1_1`。

最后，`isUnconditionalBranch` 必须同时认两种形态：

```cpp
if (MI.getOpcode() == RVMin::PseudoBR) return true;
return MI.getOpcode() == RVMin::JAL && MI.getOperand(0).getReg() == RVMin::X0;
```

因为 `analyzeBranch` 可能在伪指令展开前后被调用。

## 三个真实的坑

### 坑 1：条件分支不能标 `isBarrier`

第 1 章为了让 `ret` 不被优化掉，把整个分支组设成
`isBarrier = 1`。条件分支有 fallthrough，这个标记会让机器验证器认为
“块以 barrier 结束，不可能落到下一个块”：

```text
*** Bad machine code: MBB exits via conditional branch/fall-through but
    ends with a barrier instruction!
```

修正：条件分支只保留 `isBranch = 1, isTerminator = 1`；
`JAL/JALR`（含 `PseudoBR`）继续是 `isBarrier`。

### 坑 2：只改 `Operand<OtherVT>` 不够，谓词也要认符号

本章第一版只把 `simm13_lsb0` 的类型从 `i32` 改成 `OtherVT`，
`MCOperandPredicate` 仍然只有 `evaluateAsConstantImm`。结果：

- `llc` 能选指令；
- `llvm-mc -triple=rvmin` 汇编 `beq a0, a1, label` 时报
  `invalid operand for instruction`。

原因是 TableGen 生成的 MC 校验对每个操作数都调用谓词，符号引用
`evaluateAsConstantImm` 返回 false。加上 `MCOp.isBareSymbolRef()` 之后，
立即数和标签两种形态都能通过。

### 坑 3：`encodeJalImm` 的 `>> 12`——第 2 章埋的 bug

第 2 章写 fixup 时，这一行是：

```cpp
Inst |= (Imm & 0x0ff000u) >> 12;   // 错
```

`Imm` 已经是完整字节偏移，`imm[19:12]` 本来就该放在 bit 19:12，不需要右移。
右移 12 位后这些位全跑到 bit 11:0，跟 `rd`/`opcode` 字段重叠。

为什么第 2 章的门禁没抓到？因为 `jal 16`、`beq a0,a1,12` 这些**立即数**
操作数走的是 TableGen 生成的位域编码（`RVMinJInst` 里的
`Inst{19-12} = imm21{19-12}`），根本不经过 `encodeJalImm`。只有“目标是
符号/标签”的 fixup 路径会调用它，而第 2 章还没有标签测试。

直到本章 `llc` 第一次输出 `jal zero, .LBB1_1`，问题才暴露：disassemble
出来跳转目标完全不对，但汇编器不报错。修正后加了一个专门的
`rvmin-fixup-corpus.s`：前向/后向分支、`jal`、间隔 1 KiB 和 8 KiB 的
远目标，要求 `.text` 与参考 RISC-V 汇编器逐字节相同。

教训：**编码门禁要覆盖每一条编码路径**。立即数编码正确，不代表 fixup
编码正确；`mv`/`ret`/`j` 这些别名也各有自己的 fixup 路径。

## 门禁与验证

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop
ninja -C "$B" llc clang llvm-mc llvm-objdump llvm-readelf llvm-objcopy FileCheck

"$B/bin/llvm-lit" -sv "$W/llvm/test/CodeGen/RVMin"
# Passed: 3 (basic.ll, alu.ll, control.ll)

"$W/llvm/lib/Target/RVMin/scripts/check-rvmin-encodings.sh" "$B/bin"
# PASS: 71 instructions, .text identical (incl. label fixups), ELF machine RISC-V
```

`control.ll` 里的 `max` 输出（`-O1`）：

```text
max:
	bge a1, a0, .LBB0_2      # a > b 规范化成 b >= a，且没有 xori
	jalr zero, 0(ra)
.LBB0_2:
	addi a0, a1, 0
	jalr zero, 0(ra)
```

`count_down` 的回边：

```text
.LBB1_1:
	blt a0, a2, .LBB1_3
	add a1, a1, a0
	addi a0, a0, -1
	jal zero, .LBB1_1
```

对象级别的字节（`llvm-objdump -d --triple=riscv32`）：

```text
18: 00c54863  blt  a0, a2, 0x28
1c: 00a585b3  add  a1, a1, a0
20: fff50513  addi a0, a0, -0x1
24: ff5ff06f  j    0x18            # jal zero, .LBB1_1
```

再用 `llvm-mc` 重新汇编 `llc` 输出的 `.s`，`.text` 必须逐字节相同：

```bash
"$B/bin/llc" -march=rvmin -mtriple=riscv32-unknown-elf -O2 ch5.ll \
  -filetype=obj -o a.o
"$B/bin/llc" -march=rvmin -mtriple=riscv32-unknown-elf -O2 ch5.ll -o a.s
"$B/bin/llvm-mc" -triple=rvmin -filetype=obj a.s -o b.o
"$B/bin/llvm-objcopy" -O binary --only-section=.text a.o a.bin
"$B/bin/llvm-objcopy" -O binary --only-section=.text b.o b.bin
cmp a.bin b.bin      # 88 bytes, 一致
```

clang 端到端（含循环，`llc -O1`）：

```c
int xor_down(int n) { int s = 0; while (n > 0) { s ^= n; n -= 2; } return s; }
```

```bash
"$B/bin/clang" --target=riscv32-unknown-elf -march=rv32i -mabi=ilp32 \
  -O1 -S -emit-llvm ch5.c -o ch5.ll
"$B/bin/llc" -march=rvmin -mtriple=riscv32-unknown-elf -O1 ch5.ll -o -
```

```text
xor_down:
	addi a1, a0, 0
	addi a2, zero, 1
	addi a0, zero, 0
	blt  a1, a2, .LBB1_3
	addi a2, zero, 2
.LBB1_2:
	addi a3, a1, 0
	addi a1, a3, -2
	xor  a0, a0, a3
	blt  a2, a3, .LBB1_2
	jalr zero, 0(ra)
```

注意循环体没有 `jal`：`MachineBlockPlacement` 把回边排成了 fallthrough，
`analyzeBranch(AllowModify=true)` 删掉了多余的跳转。这正是第 6 节三个回调的意义。

## 练习

1. 现在是“条件成立跳 then、否则跳 else”。试改成先发 `bne` 跳到 else、
   再发 then，比较两种布局在 `-O2` 下的指令数与分支方向。
2. 用 `llc -print-after=block-placement` 看一个 `if/else` 的基本块顺序，
   然后手写一个 IR 让 then 块更大，观察布局是否翻转。
3. 构造一个分支距离超过 ±4 KiB 的函数（大量 `nop` 或重复 IR），确认
   `applyFixup` 的报错信息，并思考 branch relaxation 需要什么信息。

## 当前限制（第 5 章）

- **`-O0` 的多基本块函数还不行**：fast regalloc 需要把值溢出到栈上，而
  `storeRegToStackSlot` / `loadRegFromStackSlot` 第 6 章才实现。报错是
  `Target didn't implement TargetInstrInfo::storeRegToStackSlot!`。
  `-O1`…`-O3` 正常。
- `select` 仍然用第 4 章的无分支掩码序列；有了分支之后并不是必须改成
  分支形式，后续可以做 if-conversion 的对比实验。
- 分支目标只支持 ±4 KiB（B 型），没有 branch relaxation；超远的分支/跳转会
  在 `applyFixup` 报错。真实链接器在 `.text` 很大时需要
  `R_RISCV_BRANCH` 重定位 + relaxation，属于第 7 章之后的内容。
- 还没有函数调用（`ra` 被保留到第 8 章），所以 `jal` 只用来做函数内跳转；
  `R_RISCV_JAL`/`R_RISCV_CALL` 重定位第 7/8 章实现。
- `mul/div/rem` 仍未处理（第 10 章 libcall）。这也是
  `while` 循环被优化成闭式求和时会失败的原因：IR 里出现 `mul`。
