# 第 00 章：target 骨架、Triple 与空模块

## 目标

在现有 LLVM 树里新增实验 target `gpumin`，并让它成为一个**能被工具链真正
看到**的后端，而不只是一堆编译得过的文件：

1. `llc --version` 列出 `gpumin`；
2. `llc -march=gpumin` 选中 GPUMin 的 `TargetMachine`；
3. 空模块能走完 codegen 流水线并输出汇编列表；
4. `ret void` 能降到 GM32 的 `exit`（0x48000000）；
5. `llvm-mc -triple=gpumin` 能编码 `exit`；
6. 不改 RVMin、不改 clang、不新建构建目录。

退出条件（可执行）：

```sh
llvm/lib/Target/GPUMin/scripts/check-ch00.sh <llvm-bin-dir>
```

它检查四件事：target 注册、空模块出列表、`ret void` 出 `exit`、`exit`
的编码是 `0x48000000`。

## 背景

### 为什么不直接用 NVPTX

NVPTX 是 NVIDIA 的正式后端：它有完整的目标文件格式、PTX 文本、调试信息和
一套自己的 ABI。但本教程要讲的不是"如何用 NVPTX"，而是"一个 GPU 后端的
每一层是怎么长出来的"：寄存器文件、固定长度编码、`lane/warp` 的建模、
SIMT 分歧、shared memory、barrier、occupancy。

这些概念在 NVPTX 里全部埋在 PTX 虚拟 ISA 和驱动栈后面。GM32 把它们放到
台面上：32 位固定长度指令、显式的 `p0-p7` 谓词、显式的 `bar.sync`、
显式的 `laneid/warpid/blockid` 特殊寄存器。

### 为什么要有自己的 Triple arch

RVMin 第 0 章刻意**不**动 `Triple`：它用 `Triple::UnknownArch` 注册，于是
`llc -march=rvmin` 能用，但 triple 仍然是 `riscv32-unknown-unknown-elf`。

GPUMin 反过来做：`gpumin` 是一个独立的 `Triple::ArchType`。原因有三条：

1. **地址空间**。GM32 有四个内存空间（1 global、2 shared、3 private/local、
   4 constant）。如果沿用 RISC-V 的 triple，`Triple::computeDataLayout()`
   会给出 RISC-V 的布局，`p1/p2/p3/p4` 的宽度信息就丢了。
2. **避免工具链误判**。clang 看到 `riscv32-*` 会启用 RISC-V 的 ABI、内建
   函数和 `TargetInfo`；`gpumin-*` 则明确表示"这是另一个 target"。
3. **后续章节要用**。第 12 章的模拟器通过 ELF 头判断是不是 GM32；
   第 24 章（可选）要给 clang 加 `--target=gpumin`；两者都需要一个真实
   的 arch 名。

代价是要改 `Triple.h` / `Triple.cpp` / `TargetDataLayout.cpp` 的若干
switch，本章的"常见坑"里逐条列出。

## 设计

### 文件布局

```
llvm/lib/Target/GPUMin/
  CMakeLists.txt            12 个 -gen-* 目标 + LLVMGPUMinCodeGen
  GPUMin.td                 Target / Processor / AsmWriter / AsmParser
  GPUMinRegisterInfo.td     r0-r31 + GPR + SP
  GPUMinInstrInfo.td        GPUMinInst / GPUMinPseudo / EXIT / PseudoRET
  GPUMinCallingConv.td      CC_GPUMin / RetCC_GPUMin / CSR_GPUMin
  GPUMin.h, GPUMinSubtarget.{h,cpp}, GPUMinTargetMachine.{h,cpp}
  GPUMinRegisterInfo.{h,cpp}, GPUMinInstrInfo.{h,cpp}
  GPUMinFrameLowering.{h,cpp}, GPUMinISelLowering.{h,cpp}
  GPUMinISelDAGToDAG.{h,cpp}, GPUMinSelectionDAGInfo.{h,cpp}
  GPUMinAsmPrinter.{h,cpp}, GPUMinMCInstLower.{h,cpp}
  GPUMinCodeGenPassBuilder.{h,cpp}, GPUMinTargetTransformInfo.{h,cpp}
  MCTargetDesc/             MCAsmInfo / InstPrinter / MCCodeEmitter /
                            AsmBackend / ELFObjectWriter / FixupKinds
  AsmParser/                手写 parser + TableGen matcher
  Disassembler/             解码 hook（本章返回 Fail）
  TargetInfo/               RegisterTarget<Triple::gpumin>
  scripts/check-ch00.sh     本章门禁
```

共 44 个源文件、约 2400 行（含注释）。

### CodeGen 流水线的形状

GPUMin 用 **new pass manager + `CodeGenPassBuilder`**，和 RVMin 一致。
`GPUMinTargetMachine::buildCodeGenPipeline()` 把控制权交给
`buildGPUMinCodeGenPipeline()`，后者只覆盖四个钩子：

| 钩子 | 本章实现 |
| --- | --- |
| `addInstSelector` | `GPUMinDAGToDAGISelPass`（SelectionDAG） |
| `addAsmPrinterBegin` | `GPUMinAsmPrinterBeginPass` |
| `addAsmPrinter` | `GPUMinAsmPrinterPass` |
| `addAsmPrinterEnd` | `GPUMinAsmPrinterEndPass` |

AsmPrinter 之所以拆成 begin / per-function / end 三个 pass，是因为
`CodeGenPassBuilder` 把模块级初始化、逐函数发射和模块级收尾放在不同
层次上；`AsmPrinter::doInitialization/doFinalization` 不能塞进一个
MachineFunctionPass。

### 寄存器模型（本章只做骨架）

`r0-r31` 是**每 lane** 的 32 位通用寄存器，`GPR` 类的分配顺序把
caller-saved 窗口放前面，把 `r0`（zero）、`r30`（sp）、`r31`（lr）放最后。
`r0` 和 `r30` 在 `getReservedRegs()` 里保留。

uniform 寄存器（`ur0-ur7`）和谓词寄存器（`p0-p7`）留到第 01 章：
它们不是"同一寄存器文件的不同视图"，而是不同类型、不同语义的独立
寄存器类，现在塞进来只会让 `computeRegisterProperties()` 的语义变糊。

### 本章的两条指令

```tablegen
def EXIT : GPUMinInst<(outs), (ins), "exit", []> {
  let Inst{31-26} = 0x12;
  let Inst{10-0} = 0;
}

def PseudoRET : GPUMinPseudo<(outs), (ins), "", [(GPUMinRet)]>,
                PseudoInstExpansion<(EXIT)> {
  let isReturn = 1;
  let isBarrier = 1;
  let isTerminator = 1;
}
```

`EXIT` 是本章唯一的真实指令：没有操作数，整字就是
`0x12 << 26 = 0x48000000`。`PseudoRET` 是它的机器层替身，由 TableGen 的
`-gen-pseudo-lowering` 生成展开代码。

`GPUMinRet` 这个 SDNode 的存在不只是为了模式匹配：`-gen-dag-isel` 在
**没有任何 pattern** 时会生成一个零长度 `MatcherTable`，直接编译失败。
本章用一个"已经有 pattern、但只有 void 返回会产生它"的最小节点绕开这一点，
同时也让 `ret void` 真的能出汇编。

### DataLayout

`Triple::computeDataLayout()` 为 `gpumin` 返回：

```text
e-p:32:32-p1:32:32-p2:32:32-p3:32:32-p4:32:32-i8:8:8-i16:16:16-i32:32:32-f32:32:32-i64:64:64-f64:64:64-n8:16:32:64
```

四个地址空间都是 32 位指针。这一行在本章还没有可见效果（没有内存访问），
但它是第 06/16 章的前提：LLVM 在 IR 层就会用 `p2` 的宽度做类型合法化。

## 实现

### 1. Triple

- `include/llvm/TargetParser/Triple.h`：`ArchType` 加 `gpumin`，加
  `isGPUMin()`。
- `lib/TargetParser/Triple.cpp`：`getArchTypeName`、`getArchTypePrefix`、
  `parseArch`（两个表）、`getDefaultFormat`、`getArchPointerBitWidth`、
  `get32BitArchVariant`、`get64BitArchVariant`、`getBigEndianArchVariant`、
  `isLittleEndian`、`getDefaultExceptionHandling`、`getDefaultFloatABI`。
- `lib/TargetParser/TargetDataLayout.cpp`：`computeDataLayout` 的 `gpumin`
  case。

`getDefaultLongDoubleFormat()` 不加 case：`gpumin` 落到 `default` 的
`IEEEdouble`，和 DataLayout 里"最大 64 位浮点"的声明一致。

### 2. Target 注册

```cpp
// TargetInfo/GPUMinTargetInfo.cpp
RegisterTarget<Triple::gpumin> X(getTheGPUMinTarget(), "gpumin",
                                 "GPUMin (SIMT GPU teaching backend) [experimental]",
                                 "GPUMin");
```

`RegisterTarget` 的模板参数决定 **别名**：`llc -march=gpumin` 走名字查找，
`--target=gpumin-unknown-elf` 走 triple 查找，两者命中同一个 `Target`。

```cpp
// GPUMinTargetMachine.cpp
extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void LLVMInitializeGPUMinTarget() {
  RegisterTargetMachine<GPUMinTargetMachine> X(getTheGPUMinTarget());
}
```

### 3. MC 层

`LLVMInitializeGPUMinTargetMC()` 一次注册七个组件：

| 组件 | 本章实现 |
| --- | --- |
| `MCAsmInfo` | ELF、little endian、指针 4 字节、`#` 注释 |
| `MCInstrInfo` / `MCRegisterInfo` / `MCSubtargetInfo` | TableGen 生成 |
| `MCAsmBackend` | fixup `fixup_gpumin_32`；`writeNopData` 写零字 |
| `MCCodeEmitter` | `getBinaryCodeForInstr` + 4 字节小端输出 |
| `MCInstPrinter` | TableGen 生成的 `printInstruction` + 手写 `printOperand` |

### 4. AsmParser / Disassembler

Parser 已经能解析 register / expression / `imm(reg)` 三种操作数形状，
但本章的 matcher 只认 `exit`。

解码器只安装 hook，`getInstruction()` 一律返回 `Fail`。这不是偷懒：
第 02 章要在**编码器和解码器同时**按 GM32 的 R/I/M/B/J 格式展开，
那时才是解码器的正确落点。

## 代码走读

### 一次 `llc -march=gpumin ret.ll` 的路径

```text
llc
 └─ TargetRegistry::lookupTarget("gpumin", TT, ...)
     ├─ TargetRegistry::lookupTarget(ArchName)  // 名字命中
     └─ 返回 GPUMinTargetMachine
 └─ TargetMachine::buildCodeGenPipeline()
     └─ GPUMinCodeGenPassBuilder::buildPipeline()
         ├─ addIRPasses()                  // 本章用基类默认
         ├─ addInstSelector()              // GPUMinDAGToDAGISelPass
         │   └─ SelectionDAGISel
         │       ├─ LowerFormalArguments() // 无参：直接返回 chain
         │       ├─ SelectionDAG legalize  // i32 legal
         │       └─ LowerReturn()          // GPUMinISD::RET
         ├─ 通用 machine passes            // regalloc 等（空函数无副作用）
         └─ addAsmPrinter()
             └─ GPUMinAsmPrinter::emitInstruction()
                 └─ PseudoRET --(伪指令展开)--> EXIT
                     └─ MCCodeEmitter::encodeInstruction()  // 0x48000000
```

### AsmPrinter 里的伪指令展开

```cpp
if (MI->isPseudo()) {
  MCInst Expanded;
  if (lowerPseudoInstExpansion(MI, Expanded)) {
    EmitToStreamer(*OutStreamer, Expanded);
    return;
  }
  llvm_unreachable("GPUMin: unimplemented pseudo instruction");
}
```

`lowerPseudoInstExpansion` 由 `#include "GPUMinGenMCPseudoLowering.inc"`
生成。**必须**在 AsmPrinter 里先展开伪指令，否则 `printInstruction` 会
在生成的 `Bits != 0` 断言上崩掉——伪指令没有 AsmString 表项。

## 验证

```sh
$ ninja -C <build> llc llvm-mc llvm-objdump
$ llvm/lib/Target/GPUMin/scripts/check-ch00.sh <build>/bin
PASS 1/4: target registered
PASS 2/4: empty module emits a listing
PASS 3/4: ret void lowers to exit
PASS 4/4: exit encodes as 0x48000000
OK: chapter 00 gate passed
```

手工看一遍产物：

```sh
$ llc -march=gpumin ret.ll -o -
	.file	"ret.ll"
	.text
	.globl	k
	.type	k,@function
k:
	.cfi_startproc
	exit
.Lfunc_end0:
	.size	k, .Lfunc_end0-k
	.cfi_endproc

$ printf 'exit\n' | llvm-mc -triple=gpumin -show-encoding
	exit                        # encoding: [0x00,0x00,0x00,0x48]
```

`llvm-objdump -d` 本章会显示 `<unknown>`：解码器还没实现（第 02 章）。
对象文件的 machine 号暂时借用 `EM_RISCV`，以便现有 objdump 能打开它；
第 03 章会重新决定机器号并接上 GPUMin 自己的反汇编。

## 常见坑

1. **`-gen-emitter` 在零指令时直接 abort**。
   `llvm-tblgen: ArrayRef::front() ... Assertion '!empty()' failed`。
   `-gen-emitter` 需要至少一条带编码的真实指令；本章用 `EXIT` 满足它。
2. **`-gen-dag-isel` 在零 pattern 时生成零长数组**。
   报错是 `zero-size array 'MatcherTable'`。要么加 pattern，要么不要
   `DAGISel`；本章加了 `GPUMinRet` 这条最小 pattern。
3. **`.td` 没有 include guard**。`GPUMin.td` 已经 include 了
   `GPUMinRegisterInfo.td`，`GPUMinInstrInfo.td` 里再 include 一次就会
   `Class 'GPUMinReg' already defined`。
4. **`MCAsmBackend::writeNopData` 是纯虚函数**。不实现就
   `invalid new-expression of abstract class`。
5. **`MCContext::reportError` 需要 `Twine`**。`const char[N]` 不能隐式转，
   要写 `Twine("...")`，并且 `#include "llvm/ADT/Twine.h"`。
6. **`AsmMatcher` 是分段生成的**。只 `#define GET_MATCHER_IMPLEMENTATION`
   不会生成 `MatchRegisterName`，必须同时定义 `GET_REGISTER_MATCHER`，
   否则链接期 `undefined symbol: MatchRegisterName`。
7. **`MCParsedAsmOperand::print` 的签名带 `MCAsmInfo&`**，不是单参数。
8. **`TargetLowering::LowerFormalArguments` / `LowerReturn` 默认是
   "Not Implemented" 的 unreachable**。哪怕函数没有参数、没有返回值，
   也必须实现，否则 `ret void` 会在 `LowerArguments` 里崩。
9. **Triple switch 要逐个补**。漏一个会在 `-Wswitch` 或运行时
   `llvm_unreachable` 上暴露；`getArchPointerBitWidth` 那一处用的是
   `llvm::Triple::rvmin`（带命名空间），文本替换容易漏。
10. **不需要改 `LLVM_TARGETS_TO_BUILD`**，只改
    `LLVM_EXPERIMENTAL_TARGETS_TO_BUILD="RVMin;GPUMin"`；后者会被
    CMake 合并进前者。

## 练习

1. 把 `EXIT` 的 `funct` 改成 1，让 `llvm-mc -show-encoding` 输出
   `0x48000001`。想一想：为什么 `Inst{10-0}` 单独存在，而不是直接写
   `let Inst = 0x48000000`？（提示：第 01 章的格式类要靠字段名定位
   操作数。）
2. 给 `EXIT` 加一个 `InstAlias<"halt", (EXIT)>`，让汇编器同时接受
   `halt` 和 `exit`。观察 `-gen-asm-matcher` 和 `-gen-asm-writer` 的
   输出分别变了什么。
3. 在 `getReservedRegs()` 里临时取消 `r30` 的保留，编译一个
   `ret void` 的函数，看寄存器分配器会不会把 `r30` 分配出去。解释
   为什么本章还看不出来（提示：没有值需要分配）。
4. 给 DataLayout 加上 `p5:32:32`，用
   `opt -passes=verify -mtriple=gpumin-unknown-elf` 观察带
   `addrspace(5)` 的 IR 是否仍然合法。

## 下一章

第 01 章把 GM32 的寄存器与指令格式补全：`ur0-ur7`、`p0-p7`、
R/I/M/B/J 五种 32 位格式，以及 10 条手写指令。那一章的退出条件是
"TableGen 接受全部指令定义"，还不需要编码正确。
