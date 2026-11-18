# 第 03 章：最小函数与 ELF

## 目标

前两章证明了"指令能被编码、能被解码"，但 `llc` 还只会输出 `exit`。
本章让一条真实的函数走完整条管线：

1. 参数按 `CC_GPUMin` 落在 `r4-r11`，返回值按 `RetCC_GPUMin` 落在 `r4`；
2. `ret i32 %b` 真的产生 `mov r4, r5`，`ret i32 42` 产生 `movi r4, 42`；
3. 对象文件带自己的 machine 号 `EM_GPUMIN`（`0xfe00`），读回时映射成
   `Triple::gpumin`，而不是被猜成 RISC-V；
4. `llvm-objdump -d` 用 GPUMin 自己的反汇编器解码；
5. `llc` 生成的 `.o` 与"汇编 `.s` 再汇编"的 `.o` 逐字节一致。

退出条件（可执行）：

```sh
llvm/lib/Target/GPUMin/scripts/check-ch03.sh <llvm-bin-dir>
```

## 背景

### 为什么"函数"要等到第三章

第 00 章的 `ret void` 走的是 `LowerReturn` 的最短分支：没有值、没有参数、
只有一条 `EXIT`。本章第一次出现"值要跨过 ABI 边界"：参数从物理寄存器
进虚拟寄存器，返回值从虚拟寄存器回物理寄存器。这两件事各自会踩不同的坑，
而且都和"寄存器是否活着"有关。

### 为什么参数从 `r4` 开始

`r0` 是零寄存器，`r30/r31` 是栈指针和链接寄存器，都不能当参数。剩下的
窗口 `r1-r29` 里，`CC_GPUMin` 选了 `r4-r11`，对齐常见的 GPU ABI 习惯
（前几个参数在低编号寄存器里，方便编码位宽和调试）。

```tablegen
def CC_GPUMin : CallingConv<[
  CCIfType<[i32], CCAssignToReg<[R4, R5, R6, R7, R8, R9, R10, R11]>>,
  CCIfType<[i32], CCAssignToStack<4, 4>>
]>;

def RetCC_GPUMin : CallingConv<[
  CCIfType<[i32], CCAssignToReg<[R4, R5]>>
]>;
```

栈参数留给第 07 章的帧布局，本章只处理寄存器参数（`CCAssignToStack` 已经写在表里，
但 `LowerFormalArguments` 遇到 `!isRegLoc()` 会直接 `report_fatal_error`）。

### 为什么 machine 号要自己造

ELF 的 `e_machine` 是 16 位。第 00 章借了 `EM_RISCV`，于是
`llvm-objdump` 打开文件后按 RISC-V 解释，把每个 GM32 字切成 16 位
"压缩指令"，输出一堆 `<unknown>`。

本章取 `0xfe00`：它在已分配区间之上，不会和未来的 `EM_*` 冲突。代价是
三处映射要手写，收益是下面这条链路终于闭合：

```text
e_machine = 0xfe00
  -> ELFObjectFile::getArch()  -> Triple::gpumin
  -> TargetRegistry::lookupTarget("gpumin")
  -> GPUMinMCDisassembler
  -> `mov r4, r5`
```

## 设计

### 调用约定与数据流

```text
IR:  define i32 @pick_second(i32 %a, i32 %b)
          │
          ├─ LowerFormalArguments
          │    CC_GPUMin -> %a 在 r4, %b 在 r5
          │    每个物理寄存器变成一个 vreg + CopyFromReg
          │
          └─ LowerReturn
               RetCC_GPUMin -> 返回值在 r4
               CopyToReg r4, %b
               RET(chain, glue, r4)
```

### RET 节点必须带寄存器操作数

这是本章唯一一个"写错了会静默出错"的地方。如果 `LowerReturn` 只把
chain 传给 RET：

```cpp
// 错误写法：机器层看不出 r4 被使用
return DAG.getNode(GPUMinISD::RET, DL, MVT::Other, Chain);
```

生成的 MIR 其实是正确的：

```text
liveins: $r5
%1:gpr = COPY $r5
$r4 = COPY %1
PseudoRET
```

但 `PseudoRET` 没有操作数，机器层的活跃性分析找不到任何读 `r4` 的地方，
`dead-mi-elimination` 就把 `$r4 = COPY %1` 删了——函数"返回"的是
`r4` 里恰好剩下的垃圾。正确写法是把寄存器作为 RET 的变长操作数：

```cpp
RetOps.push_back(DAG.getRegister(VA.getLocReg(), VA.getLocVT()));
```

`SDTNone + SDNPVariadic` 的 RET 节点接受这些操作数；它们不进最终指令，
但让 DAG 和机器层都知道"这个寄存器在出口是活的"。

### 常量只在 16 位内

`movi` 的立即数是 16 位有符号。本章在 `Select()` 里做范围检查：

```cpp
if (N->getOpcode() == ISD::Constant) {
  int64_t V = cast<ConstantSDNode>(N)->getSExtValue();
  if (isInt<16>(V)) { /* 生成 MOVI */ }
}
```

不用 pattern 的原因很直接：`(set GPR:$rd, imm:$imm)` 会匹配任意 32 位
常量，然后被 `simm16` 截断成低 16 位。范围检查必须在选择阶段做，失败
信息才明确。宽常量（`movi` + `shl` 或两条 `movi`）是第 04 章的内容。

## 实现

### 1. `LowerFormalArguments`

```cpp
SmallVector<CCValAssign, 16> ArgLocs;
CCState CCInfo(CallConv, IsVarArg, MF, ArgLocs, *DAG.getContext());
CCInfo.AnalyzeFormalArguments(Ins, CC_GPUMin);

for (CCValAssign &VA : ArgLocs) {
  Register VReg = RegInfo.createVirtualRegister(&GPUMin::GPRRegClass);
  RegInfo.addLiveIn(VA.getLocReg(), VReg);
  InVals.push_back(DAG.getCopyFromReg(Chain, DL, VReg, VA.getLocVT()));
  Chain = InVals.back().getValue(1);
}
```

`addLiveIn` 把"函数入口时 `r5` 已经有值"这件事写进 `MachineFunction`；
没有它，寄存器分配器会认为 `r5` 在入口是未定义的。

### 2. `LowerReturn`

```cpp
SmallVector<SDValue, 4> RetOps(1, Chain);
SDValue Glue;
for (CCValAssign &VA : RVLocs) {
  Chain = DAG.getCopyToReg(Chain, DL, VA.getLocReg(), OutVals[i], Glue);
  Glue = Chain.getValue(1);
  RetOps.push_back(DAG.getRegister(VA.getLocReg(), VA.getLocVT()));
}
RetOps[0] = Chain;
if (Glue)
  RetOps.push_back(Glue);
return DAG.getNode(GPUMinISD::RET, DL, MVT::Other, RetOps);
```

`Glue` 的判空不是防御性编程：void 返回时 `Glue` 是默认构造的
`SDValue()`，把它塞进 `getNode` 会解引用空节点——本章实现时就是一个
段错误。

### 3. `copyPhysReg`

```cpp
if (!GPUMin::GPRRegClass.contains(DestReg, SrcReg))
  llvm_unreachable("GPUMin: only GPR-to-GPR copies are supported");

BuildMI(MBB, I, DL, get(GPUMin::MOV), DestReg)
    .addReg(SrcReg, getKillRegState(KillSrc));
```

调用约定产生的 `COPY` 从这里变成 `mov`。第 19 章加入 spill 之后，同一个
钩子还要负责 `spill/reload`；本章它只处理寄存器到寄存器。

### 4. ELF 的三处映射

| 文件 | 改动 |
| --- | --- |
| `BinaryFormat/ELF.def` | `ELF_MACHINE(EM_GPUMIN, 0xfe00, "gpumin", ...)` |
| `Object/ELFObjectFile.h` | `getArch()` 与 `getFileFormatName()` 各加一个 case |
| `GPUMinELFObjectWriter.cpp` | `MCELFObjectTargetWriter(..., ELF::EM_GPUMIN, ...)` |

`getArch()` 决定 objdump 选哪个 `Target`；`getFileFormatName()` 只影响
readobj/objdump 打印的 `elf32-...` 文本。两个都要加：只加前者时
readobj 会显示 `Format: elf32-unknown`，看起来像没生效。

## 代码走读

### `llc ch03.ll`

```text
define i32 @pick_second(i32 %a, i32 %b) { ret i32 %b }
  │
  ├─ SelectionDAGBuilder
  │    %a -> CopyFromReg r4, %b -> CopyFromReg r5
  │    return -> CopyToReg r4, %b; RET(r4)
  │
  ├─ SelectionDAGISel
  │    COPY 节点由寄存器分配前的 coalescer 处理
  │    RET -> PseudoRET
  │
  ├─ 机器层
  │    liveins: $r5
  │    %1:gpr = COPY $r5
  │    $r4 = COPY %1
  │    PseudoRET
  │
  └─ AsmPrinter
       PseudoRET -> EXIT
       COPY -> mov（copyPhysReg）
```

最终：

```asm
pick_second:
	mov r4, r5
	exit

forty_two:
	movi r4, 42
	exit

k:
	exit
```

### 对象文件

```sh
$ llc -march=gpumin -filetype=obj ch03.ll -o ch03.o
$ llvm-readelf -h ch03.o
  Machine:                           GPUMin teaching GPU
$ llvm-readobj -h ch03.o | head -4
  Format: elf32-gpumin
  Arch: gpumin
$ llvm-objdump -d ch03.o
       0: 00 00 85 04  	mov r4, r5
       4: 00 00 00 48  	exit
```

`readobj` 的 `Machine:` 一栏显示 `EM_GPUMIN (0xFE00)`，`readelf` 才打印
`ELF.def` 里的描述字符串——两个工具的取名字段不同，不是数据不一致。

## 验证

```sh
$ llvm/lib/Target/GPUMin/scripts/check-ch03.sh <build>/bin
PASS 1/4: arguments, returned constants and returned values lower correctly
PASS 2/4: the object file is elf32-gpumin and reads back as gpumin
PASS 3/4: llvm-objdump -d decodes the object file
PASS 4/4: llc's .o and the reassembled .s are identical
OK: chapter 03 gate passed
```

第 4 项比较的是两个 `.o` 的反汇编文本，而不是文件字节：ELF 的段名、
对齐填充可能不同，但 `.text` 的内容必须一致，否则说明 `llc` 走了和
汇编器不同的编码路径。

## 常见坑

1. **RET 不带寄存器操作数 → 返回值的拷贝被 DCE 删掉**。症状是反汇编里
   只有 `exit`，没有任何 `mov`，而 MIR 阶段看起来完全正确。这是本章
   最贵的一个坑：`-stop-after=finalize-isel` 之后才能看到删除动作。
2. **void 返回时 `Glue` 是空 `SDValue`**。直接塞进 `getNode` 会段错误，
   栈回溯指向 `SelectionDAG::getNode`，看不出真正原因。先判空。
3. **`#define GET_CALLING_CONV_IMPL` 才能 include `GenCallingConv.inc`**。
   忘了定义会得到三条 `'CC_GPUMin' was not declared`，看起来像 .td 没
   生成，其实是 include 被 `#ifdef` 挡掉了。
4. **`CCState` 接收的是 `LLVMContext &`**，不是 `LLVMContext *`；
   同理 `AnalyzeFormalArguments` 需要 `Ins` 的 `ISD::InputArg` 列表，
   不是 IR 参数列表。
5. **常量 pattern 会静默截断**。`movi` 的 `simm16` 只检查汇编语法；
   用它匹配任意 `Constant` 会把 `70000` 变成 `4464`。范围检查放在
   `Select()`。
6. **ELF machine 号要同时改 `getArch()` 和 `getFileFormatName()`**。
   只改一个时 objdump 能反汇编但 readobj 显示 `elf32-unknown`，
   很容易误判成"没生效"。
7. **`.o` 一致性对比要过滤 `file format` 行**。objdump 的输出第一行
   带文件路径，直接 `diff` 会在这一行失败，和编码无关。

## 练习

1. 加第三个参数，观察它落在 `r6`。把参数改成 9 个，看第 9 个触发哪条
   `report_fatal_error`（提示：`CCAssignToStack` 已经生效，但
   `LowerFormalArguments` 还没实现栈参数）。
2. 写 `define i32 @big() { ret i32 70000 }`，用 `-debug-only=gpumin-isel`
   观察 `Select()` 的日志，再解释为什么最终是
   `Cannot select: ... Constant<70000>`。
3. 把 `EM_GPUMIN` 换成 `273`，重新生成 `.o`，对比 readobj/objdump 的
   行为。说明为什么"占用一个尚未分配的编号"在教程树里是可以接受的，
   但在上游贡献里不行。
4. 用 `llc -stop-after=finalize-isel` 导出 MIR，手工删掉 `$r4 = COPY %1`
   再跑 `-start-after=finalize-isel`，观察机器码的变化。这条练习是
   第 1 个坑的反向演示。
5. 把 `RetCC_GPUMin` 改成 `r4-r5` 以外的寄存器，重跑 gate，列出哪些
   断言会失败，解释"ABI 写在两处"（`.td` 和 lowering 的 assumptions）
   为什么容易漂移。

## 下一章

第 04 章补齐整数 ALU：`add/sub/and/or/xor/shl/shr/sar` 的 SelectionDAG
模式、宽常量物化、以及 `setp`/`selp` 的 lowering。到那时 `.ll` 里
出现算术表达式才会真正生成指令，而不是停在 `Cannot select`。
