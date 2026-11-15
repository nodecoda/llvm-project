# 第 3 章：最小指令选择与栈帧骨架

## 目标

前两章只有“指令长什么样”和“怎么变成字节”，没有任何东西把 LLVM IR 变成
MachineInstr。本章接上 SelectionDAG，让下面两个函数真正产出 RV32I 汇编：

```c
int f(void)          { return 42; }
int add(int a, int b) { return a + b; }
```

期望输出：

```asm
f:
        addi    a0, zero, 42
        jalr    zero, 0(ra)
add:
        add     a0, a1, a0
        jalr    zero, 0(ra)
```

本章只做三件事：**传参**（a0–a7）、**返回**（a0/a1）、**算术/常量的最小模式**。
调用、栈、控制流分别在第 8、6、5 章。

## 数据流全景

```text
LLVM IR
  │  SelectionDAGBuilder           （通用代码，不用我们写）
  ▼
SelectionDAG  ── LegalizeTypes / LegalizeDAG ──（通用）
  │  RVMinTargetLowering：LowerFormalArguments / LowerReturn
  ▼
SelectionDAG（含 RVMinISD::RET_FLAG）
  │  RVMinDAGToDAGISel：TableGen 模式 + 手写 Select 钩子
  ▼
MachineInstr（含 PseudoRET 等伪指令）
  │  通用后端：调度、寄存器分配、PEI
  ▼
RVMinAsmPrinter → MCInst（PseudoRET 在此展开成 JALR）
  ▼
RVMinMCCodeEmitter → 4 字节小端机器码
```

## 代码地图

| 文件 | 作用 |
| --- | --- |
| `RVMinCallingConv.td` | `CC_RVMin` / `RetCC_RVMin` 声明式 ABI |
| `RVMinISelLowering.{h,cpp}` | `LowerFormalArguments`、`CanLowerReturn`、`LowerReturn` |
| `RVMinISelDAGToDAG.{h,cpp}` | `SelectionDAGISel` 子类与 pass 包装；本章只转发给 TableGen matcher |
| `RVMinInstrInfo.td` | `RVMinRet` SDNode、`PseudoRET`、常量/add 模式 |
| `RVMinSelectionDAGInfo.{h,cpp}` | 生成的 SDNode 描述（DAG 校验与 dump 用） |
| `RVMinAsmPrinter.cpp` | 伪指令展开（`-gen-pseudo-lowering`） |
| `RVMinCodeGenPassBuilder.cpp` | 用真正的 `RVMinDAGToDAGISelPass` 替换第 0 章的占位 fatal pass |
| `llvm/test/CodeGen/RVMin/basic.ll` | 本章 FileCheck 门禁 |

## 关键实现

### 1. 调用约定用 TableGen 描述

`RVMinCallingConv.td`：

```tablegen
def CC_RVMin : CallingConv<[
  CCIfType<[i32], CCAssignToReg<[X10, X11, X12, X13, X14, X15, X16, X17]>>,
  CCIfType<[i32], CCAssignToStack<4, 4>>
]>;

def RetCC_RVMin : CallingConv<[
  CCIfType<[i32], CCAssignToReg<[X10, X11]>>
]>;
```

`X10..X17` 就是 AAPCS 里的 `a0..a7`。C++ 侧只需要：

```cpp
CCState CCInfo(CallConv, IsVarArg, MF, ArgLocs, *DAG.getContext());
CCInfo.AnalyzeFormalArguments(Ins, CC_RVMin);
```

`RVMin.td` 里要 `include "RVMinCallingConv.td"`，并生成
`RVMinGenCallingConv.inc`（`-gen-callingconv`）。注意 `.cpp` 里必须先
`#define GET_CALLING_CONV_IMPL` 再 include。

### 2. 形参：CopyFromReg

```cpp
Register VReg = RegInfo.createVirtualRegister(&RVMin::GPRRegClass);
RegInfo.addLiveIn(VA.getLocReg(), VReg);
SDValue ArgValue = DAG.getCopyFromReg(Chain, DL, VReg, LocVT);
```

`addLiveIn` 告诉寄存器分配“函数入口这个物理寄存器已经是活的”。
第 6 章之前不支持栈传参，遇到 `!VA.isRegLoc()` 直接 `report_fatal_error`，
把问题暴露在正确的地方。

### 3. 返回：RET_FLAG + PseudoRET

先声明一个 target SDNode：

```tablegen
def RVMinRet : SDNode<"RVMinISD::RET_FLAG", SDTNone,
                      [SDNPHasChain, SDNPOptInGlue, SDNPVariadic]>;
```

`LowerReturn` 把返回值 `CopyToReg` 到 a0，然后：

```cpp
SmallVector<SDValue, 4> RetOps(1, Chain);
// ... RetOps.push_back(DAG.getRegister(VA.getLocReg(), VA.getLocVT()));
RetOps[0] = Chain;
if (Glue.getNode())
  RetOps.push_back(Glue);
return DAG.getNode(RVMinISD::RET_FLAG, DL, MVT::Other, RetOps);
```

`SDNPVariadic` 是必需的：DAG 校验允许任意多个 `Register` 操作数跟在 chain 后面。
`SDNPOptInGlue` 允许最后一个操作数是 glue（前面 `CopyToReg` 产生的依赖链）。

伪指令本体：

```tablegen
def PseudoRET : RVMinPseudo<(outs), (ins), "", [(RVMinRet)]>,
                PseudoInstExpansion<(JALR X0, X1, 0)> {
  let isReturn = 1;
  let isBarrier = 1;
  let isTerminator = 1;
  let Size = 4;
}
```

- 模式 `[(RVMinRet)]` 让 TableGen matcher 在 `SelectCode` 里选中它；
- `PseudoInstExpansion` 让 `-gen-pseudo-lowering` 生成
  `lowerPseudoInstExpansion()`，汇编/编码时展开成真正的 `jalr zero, 0(ra)`；
- 第 2 章已经写好的编码器因此可以直接复用，不需要为伪指令写编码逻辑。

`RVMinAsmPrinter::emitInstruction` 的展开入口：

```cpp
if (MI->isPseudo()) {
  MCInst Expanded;
  if (lowerPseudoInstExpansion(MI, Expanded)) {
    OutStreamer->emitInstruction(Expanded, getSubtargetInfo());
    return;
  }
  llvm_unreachable("RVMin: unimplemented pseudo instruction ...");
}
```

### 4. 本章的指令选择模式

```tablegen
def : Pat<(i32 simm12:$imm), (ADDI X0, simm12:$imm)>;
def : Pat<(add GPR:$rs1, GPR:$rs2), (ADD GPR:$rs1, GPR:$rs2)>;
def : Pat<(add GPR:$rs1, simm12:$imm), (ADDI GPR:$rs1, simm12:$imm)>;
```

`(i32 simm12:$imm)` 把小常数物化成 `addi rd, zero, imm`。超过 12 位的常量要等
第 4 章的 `LUI + ADDI`，本章会以“无法选择”的形式暴露出来（这是刻意的）。

### 5. 接入 pass pipeline

第 0 章的 `addInstSelector` 是个 fatal 占位；本章替换成：

```cpp
Error RVMinCodeGenPassBuilder::addInstSelector(PassManagerWrapper &PMW) {
  addMachineFunctionPass(RVMinDAGToDAGISelPass(getTM()), PMW);
  return Error::success();
}
```

`RVMinDAGToDAGISel` 需要自己保存 subtarget（本树的 `SelectionDAGISel` 没有
`Subtarget` 成员），并 include `RVMinGenDAGISel.inc`：

```cpp
class RVMinDAGToDAGISel : public SelectionDAGISel {
  const RVMinSubtarget *Subtarget = nullptr;
public:
  bool runOnMachineFunction(MachineFunction &MF) override {
    Subtarget = &MF.getSubtarget<RVMinSubtarget>();
    return SelectionDAGISel::runOnMachineFunction(MF);
  }
#include "RVMinGenDAGISel.inc"
};
```

手写 `Select()` 目前只处理“已经是机器指令”的情况，其余全部交给生成的 matcher；
第 4 章开始在这里加模式表达不了的选择。

## 两个真实的坑

### 坑 1：定义了 target SDNode 却没有 `SelectionDAGTargetInfo` → 段错误

现象：第一个 `RET_FLAG` 节点插入 DAG 时崩溃在
`SelectionDAG::verifyNode`，栈里是 `getNode(...)` ← `LowerReturn`。

原因：

```cpp
// llvm/include/llvm/CodeGen/TargetSubtargetInfo.h
virtual const SelectionDAGTargetInfo *getSelectionDAGInfo() const {
  return nullptr;   // 默认值
}
```

DAG 校验对 target opcode 会调用 `getSelectionDAGInfo().verifyTargetNode(...)`，
默认返回 `nullptr` 就会被解引用。

修复：像 MSP430 那样补一个 `SelectionDAGGenTargetInfo` 子类，并生成
`RVMinGenSDNodeInfo.inc`：

```cmake
llvm_tablegen(RVMinGenSDNodeInfo.inc -gen-sd-node-info)
```

```cpp
// RVMinSelectionDAGInfo.h
#define GET_SDNODE_ENUM
#include "RVMinGenSDNodeInfo.inc"
class RVMinSelectionDAGInfo : public SelectionDAGGenTargetInfo { ... };

// RVMinSubtarget.cpp
TSInfo = std::make_unique<RVMinSelectionDAGInfo>();
```

顺带的好处：`RVMinISD::RET_FLAG` 枚举由 TableGen 生成，不再手写
（手写会和生成物重复定义，编译期直接报 `RET_FLAG conflicts with a previous
declaration`）。

### 坑 2：`TargetFrameLowering` 的 `LocalAreaOffset` 让空函数也有栈帧

现象：`f` 没有任何栈对象，却走进了 `emitPrologue` 的
“stack frames are not implemented yet” 分支。

原因：构造函数写成了 `TargetFrameLowering(StackGrowsDown, Align(16), -4, ...)`，
`-4` 是 local area offset，PEI 会把它算进 frame size。RISC-V/MSP430/Lanai 这类
没有固定本地区的目标都传 `0`：

```cpp
TargetFrameLowering(StackGrowsDown, Align(16), /*LocalAreaOffset=*/0, Align(16))
```

## 门禁与验证

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop

cmake -S "$W/llvm" -B "$B" -DLLVM_EXPERIMENTAL_TARGETS_TO_BUILD=RVMin
ninja -C "$B" llc clang llvm-mc llvm-objdump llvm-readelf FileCheck

# FileCheck 门禁
"$B/bin/llvm-lit" -sv "$W/llvm/test/CodeGen/RVMin/basic.ll"
# Passed: 1

# 手工看汇编
"$B/bin/llc" -march=rvmin -mtriple=riscv32-unknown-elf /tmp/rvmin-ch3.ll -o -
```

clang 端到端（本章的两个函数）：

```bash
"$B/bin/clang" --target=riscv32-unknown-elf -march=rv32i -mabi=ilp32 \
  -O1 -S -emit-llvm ch3.c -o ch3.ll
"$B/bin/llc" -march=rvmin -mtriple=riscv32-unknown-elf ch3.ll -o -
# addi a0, zero, 42 ; jalr zero, 0(ra)
# add  a0, a1, a0   ; jalr zero, 0(ra)
```

字节级回读（clang 生成的 C 函数带 `nounwind`，见“当前限制”）：

```bash
"$B/bin/llc" -march=rvmin -mtriple=riscv32-unknown-elf ch3.ll \
  -filetype=obj -o ch3.o
"$B/bin/llvm-objdump" -d --triple=riscv32 ch3.o
# 0: 02a00513  li   a0, 0x2a
# 4: 00008067  ret
```

这两条与参考 RISC-V 后端逐字节一致（`li a0, 0x2a` / `ret` 是参考汇编器的
别名，编码就是 `addi a0, zero, 42` / `jalr zero, 0(ra)`）。

## 练习

1. 把 `RET` 也改成伪指令 + `PseudoInstExpansion`（像 `PseudoBR` 那样），
   用 `llc -stop-after=finalize-isel` 对比 MIR 里的差异。
2. 加一条 `mv rd, rs` 伪指令（展开成 `addi rd, rs, 0`），在
   `RVMinInstrInfo.td` 里用 `PseudoInstExpansion` 实现，并让汇编器接受它。
3. 把返回值寄存器从 `a0`（`x10`）改成 `a5`，观察调用约定表
   `RVMinCallingConv.td` 与 `LowerReturn` 需要一起改哪些地方。

## 当前限制（第 3 章）

- 只支持寄存器传参；栈传参、被调用者保存、`ra` 保存等第 6/8 章补齐。
- 没有函数调用：调用相关 lowering 第 8 章实现。
- 大常量（超出 12 位）和大部分 ALU 运算尚未选择，第 4 章补齐。
- **对象文件**：手写 IR 若没有 `nounwind`，LLVM 会生成 `.eh_frame`，其中对
  函数符号的 PC-relative 引用需要重定位；重定位第 7 章才实现，因此现在会报
  `RVMin: relocations are not implemented yet (chapter 7)`。产 asm（`-o -`）
  不受影响；clang 的 C 函数默认带 `nounwind`，可以直接出 `.o`。
