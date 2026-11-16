# 第 18 章：跳转表与间接跳转

## 目标

第 11 章把 `switch` 固定成比较链，因为当时 RVMin 既没有跳转表，也没有间接跳转：

```cpp
setOperationAction(ISD::BR_JT, MVT::Other, Expand);
setOperationAction(ISD::BRIND, MVT::Other, Expand);
```

本章补上这两块：

```c
int dense(int x) {            // 8 个连续 case
  switch (x) {
  case 0: return 100;
  ...
  case 7: return 107;
  default: return -1;
  }
}
```

完成本章后：

1. 稠密 `switch` 在 `-O0`…`-O3` 都变成跳转表（表体在 `.rodata`）；
2. 间接跳转是单条 `jalr zero, 0(rs)`，与尾调用的编码相同、语义不同（不写 `ra`）；
3. 表项是 `.word .LBB*`，由已有的 `R_RISCV_32` 在链接时解析，不需要新重定位；
4. 稀疏 `switch` 仍然是比较链，`-O2` 的代价模型会自己选择；
5. `scripts/rt/switch.c` 在 QEMU 中验证 dense / sparse / fallthrough / nested 四种形状。

## 背景：`switch` 在 SelectionDAG 里怎么选形态

`SelectionDAGBuilder::visitSwitch` 有三种候选：

| 形态 | 触发条件 | 本章之前 |
| --- | --- | --- |
| 位测试 + 分支 | case 是 2 的幂等 | 可用 |
| 比较链 | case 稀疏、数量少 | 可用（唯一可用） |
| 跳转表 | case 稠密，且目标能 lowering `BR_JT` 与 `BRIND` | **不可用** |

它判断“能不能建表”的开关就是两条 `setOperationAction`：

```cpp
setOperationAction(ISD::BR_JT, MVT::Other, Custom);   // 本章
setOperationAction(ISD::BRIND, MVT::Other, Legal);    // 本章
```

只要 `BR_JT` 还是 `Expand`，`visitSwitch` 就不会建表，连 `-O3` 也只会得到比较链。
这是第 11 章“每个优化级别都是比较链”的真正原因。

建表之后，DAG 里出现的是：

```text
BR_JT chain, (JumpTable i32 %jti), index
```

`JumpTable` 节点只带编号；表体与表项在 MachineFunction 的
`MachineJumpTableInfo` 里，最后由**通用 AsmPrinter** 发射，不需要目标自己写。

## 代码地图

| 位置 | 职责 |
| --- | --- |
| `RVMinISelLowering.cpp:139-140` | `BR_JT → Custom`、`BRIND → Legal`（本章的开关） |
| `RVMinISelLowering.cpp:170` | `LowerOperation` 分派 `BR_JT` |
| `RVMinISelLowering.cpp:254-274` | `lowerBR_JT`：表地址 + 索引×4 + load + `BRIND` |
| `RVMinISelDAGToDAG.cpp:244-253` | 把 `ISD::TargetJumpTable` 选成 `LUI+ADDI`（`%hi/%lo`） |
| `RVMinInstrInfo.td:603-615` | `PseudoBRIND` 与 `(brind GPR)` pattern |
| `RVMinMCInstLower.cpp:64-82` | `MO_JumpTableIndex` → `%hi/.LJTI`/`%lo/.LJTI` 表达式 |
| `RVMinAsmPrinter.cpp:72` | 把 AsmPrinter 传给 MC lowering（`GetJTISymbol` 要用） |
| `AsmPrinter.cpp:3548-3568` | 通用表体发射：`.LJTI0:` 标签 + 每项 `.word <block>` |
| `MCTargetDesc/RVMinMCCodeEmitter.cpp:88-108` | specifier → `fixup_rvmin_hi20/lo12_i` |
| `MCTargetDesc/RVMinELFObjectWriter.cpp:44-53` | fixup → `R_RISCV_HI20`/`LO12_I`/`32` |

## 关键实现

### 1. `lowerBR_JT`：五条指令

```cpp
SDValue TableAddr = DAG.getTargetJumpTable(JT->getIndex(), PtrVT);

SDValue Scale  = DAG.getConstant(2, DL, PtrVT);            // 每项 4 字节
SDValue Offset = DAG.getNode(ISD::SHL, DL, PtrVT, Index, Scale);
SDValue EntryAddr = DAG.getNode(ISD::ADD, DL, PtrVT, TableAddr, Offset);
SDValue Entry = DAG.getLoad(PtrVT, DL, Chain, EntryAddr,
                            MachinePointerInfo::getJumpTable(MF));
return DAG.getNode(ISD::BRIND, DL, MVT::Other, Entry.getValue(1), Entry);
```

`Index` 已经被 `visitSwitch` 归一化成 0 基；这里只做三件事：取地址、缩放、加载。
`MachinePointerInfo::getJumpTable` 让 alias 分析知道这次 load 读的是只读的表，
而不是任意内存。

### 2. `TargetJumpTable` 必须自己选

`ISD::TargetJumpTable` 不是普通常量：它要变成 `%hi/.LJTI0` 这样的**重定位表达式**。
RVMin 的全局地址已经在 `RVMinISelDAGToDAG.cpp` 里手工物化，这里照抄同一形状：

```cpp
if (N->getOpcode() == ISD::TargetJumpTable) {
  unsigned JTI = cast<JumpTableSDNode>(N)->getIndex();
  SDValue Hi = CurDAG->getTargetJumpTable(JTI, MVT::i32, RVMinII::MO_HI20);
  SDValue Lo = CurDAG->getTargetJumpTable(JTI, MVT::i32, RVMinII::MO_LO12_I);
  SDValue Lui = SDValue(CurDAG->getMachineNode(RVMin::LUI, DL, MVT::i32, Hi), 0);
  ReplaceNode(N, CurDAG->getMachineNode(RVMin::ADDI, DL, MVT::i32, Lui, Lo));
  return;
}
```

**顺序很重要**：这段必须在 `SelectCode(N)` 之前。否则 TableGen 的
`(brind GPR:$rs1)` 会先把 `BRIND` 匹配掉，而它的地址操作数还停在
`%jump-table.0` 上 —— 这正是本章第一版 `llc` 的崩溃方式：

```text
*** Bad machine code: Expected a register operand. ***
instruction: $x10 = ADD %jump-table.0, killed $x10
```

`uimm20` 的 `OperandType` 是 `OPERAND_UNKNOWN`，所以 `MO_JumpTableIndex`
可以直接挂在 `LUI/ADDI` 上；上游 RISCV 也是用 `(LUI tjumptable:$in)`。

### 3. MC 层：`MO_JumpTableIndex` 需要 AsmPrinter

`.LJTI0` 是 `MachineJumpTableInfo` 的符号，名字由 AsmPrinter 维护。因此
`LowerRVMinMachineInstrToMCInst` 多了一个参数：

```cpp
MCSymbol *Sym = AP.GetJTISymbol(MO.getIndex());      // .LJTI0
const MCExpr *Expr = MCSymbolRefExpr::create(Sym, Ctx);
// MO_HI20 → S_HI20, MO_LO12_I → S_LO12_I
```

`GetJTISymbol` 的第二个参数 `isLinkerPrivate` 必须与 `emitJumpTableInfo`
里的调用一致（ELF 路径是默认 `false`），否则会拿到两个不同名字的符号。

之后的路径完全是第 7 章已有的：specifier → fixup → ELF 重定位。

### 4. `PseudoBRIND` 是“分支”，不是“返回”

```tablegen
def PseudoBRIND : RVMinPseudo<(outs), (ins GPR:$rs1), "", []>,
                  PseudoInstExpansion<(JALR X0, GPR:$rs1, 0)> {
  let isBranch = 1;
  let isBarrier = 1;
  let isTerminator = 1;
  let isIndirectBranch = 1;   // ← 关键
  let Size = 4;
}
```

`isIndirectBranch` 不只是给分析器看的。AsmPrinter 判断“这个块是否只靠
fallthrough 到达”时，会检查前驱的终止指令：

```cpp
if (!MI.isBranch() || MI.isIndirectBranch())
  return false;             // 不是 fallthrough
```

少了这个 flag，布局上紧跟在间接跳转后面的第一个表目标（本例 `.LBB0_2`）
会被当成 fallthrough 块，**不打印标签**，于是表项 `.word .LBB0_2` 引用了一个
未定义符号：

```text
error: Undefined temporary symbol .LBB0_2
```

### 5. 表体由通用代码发射

`AsmPrinter::emitJumpTableInfo` 为每个表：

1. 切到 `.rodata`（`getSectionForJumpTable`）；
2. 打 `.LJTI0_0:` 标签与 `.type/.size`；
3. 每个表项发一条 `.word .LBB0_x`（`EK_BlockAddress`）。

因此本章**一行表体代码都不用写**。链接时 8 条 `R_RISCV_32` 把地址填成绝对地址。

## 构建与验证

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop
ninja -C "$B" llc llvm-mc llvm-objdump llvm-readelf clang

# 1. 本章 lit（-O2 表 + -O0 表 + 稀疏链 + 重定位 + llvm-mc 往返）
"$B/bin/llvm-lit" -v "$W/llvm/test/CodeGen/RVMin/jump-table.ll"

# 2. 全部门禁（lit / 编码 / e2e / -verify-machineinstrs）
"$W/llvm/lib/Target/RVMin/scripts/rvmin-verify-all.sh" "$B/bin"

# 3. QEMU：switch 程序（dense/sparse/fallthrough/nested）
"$W/llvm/lib/Target/RVMin/scripts/rvmin-run.sh" "$B/bin"

# 4. 手工观察
"$B/bin/llc" -march=rvmin -mtriple=riscv32-unknown-elf -O2 switch.ll -o switch.s
grep -n "LJTI\|jalr zero, 0(" switch.s
"$B/bin/llvm-mc" -triple=rvmin -filetype=obj switch.s -o switch.o
"$B/bin/llvm-readelf" -r switch.o
"$B/bin/llvm-objdump" -d switch.o
```

## 预期输出

`-O2` 的核心序列（寄存器分配后，`lui/addi` 可能被排到 `slli` 前）：

```asm
	addi	a1, zero, 7
	bltu	a1, a0, .LBB0_10
	slli	a0, a0, 2
	lui	a1, %hi(.LJTI0_0)
	addi	a1, a1, %lo(.LJTI0_0)
	add	a0, a1, a0
	lw	a0, 0(a0)
	jalr	zero, 0(a0)
.LBB0_2:                       # 第一个表目标必须有标签
	addi	a0, zero, 100
	jalr	zero, 0(ra)
```

表体：

```asm
	.section	.rodata,"a",@progbits
	.p2align	2, 0x0
	.type	.LJTI0_0,@object
.LJTI0_0:
	.word	.LBB0_2
	...
	.word	.LBB0_9
	.size	.LJTI0_0, 32
```

重定位：

```text
.rela.text:   R_RISCV_HI20   .rodata + 0
              R_RISCV_LO12_I .rodata + 0
.rela.rodata: 8 × R_RISCV_32  .text + <block 偏移>
```

`llvm-objdump -d`（借用参考 RISC-V 反汇编器，第 20 章才有自己的）：

```text
      18: 00052503      lw   a0, 0x0(a0)
      1c: 00050067      jr   a0
```

QEMU 门禁：

```text
PASS: jump-table switch program ran under qemu-riscv32-static
```

## 常见坑

### 坑 1：只改 `setOperationAction` 就以为完成

`BR_JT`/`BRIND` 只是**允许**建表；`TargetJumpTable` 的选择与
`MO_JumpTableIndex` 的 lowering 都要自己补。少了任一个，`llc` 会在
MachineVerifier 里 abort，而不是给出“不会”的诊断。

### 坑 2：`isIndirectBranch` 缺失

症状是汇编阶段 `Undefined temporary symbol .LBB0_2`，但 lit 的 `FileCheck`
只要不检查第一项就发现不了。本章的测试专门 CHECK 了 `.LBB0_2:`。

### 坑 3：`GetJTISymbol` 的 `isLinkerPrivate` 不一致

发射标签用默认 `false`，取符号也必须用默认值；传 `true` 会生成 `.LJTI0`
之外的另一个符号，汇编直接报未定义。

### 坑 4：以为 `-O0` 一定走比较链

`switch` 的形态选择发生在 SelectionDAGBuilder，与 `-O` 级别无关：
`llc -O0` 照样建表（本章 lit 的 `O0` 前缀就是证据）。
IR 层的 `LowerSwitch` pass 不参与这条路径。

### 坑 5：稀疏 `switch` 也想建表

`visitSwitch` 会按 case 密度与数量估算代价。4 个相距很远的 case
（`-1000/7/42/999`）仍然是比较链；这是正确行为，不是本章失效。

## 练习

1. 把 `dense` 扩到 16 个 case，确认表项变成 16 条 `.word`，并观察代码尺寸变化。
2. 把 default 改成“落到某个 case”，看 `visitSwitch` 是否省掉上界检查。
3. 用 `-print-after=finalize-isel` 对比 dense 与 sparse 的 MIR，找到建表决策点。
4. 试着把 `PseudoBRIND` 的 `isBarrier` 去掉，看块布局与表项顺序如何变化（记得改回）。
5. 给 `jump-table.ll` 加一个 `-O3` RUN 行，确认优化级别不影响建表。

## 下一章

第 19 章做常量乘除的强度削减：`x/7` 不再调用 `__divsi3`，`x*3` 变成
`x + (x<<1)`，并给出有 M / 无 M 的对比。
