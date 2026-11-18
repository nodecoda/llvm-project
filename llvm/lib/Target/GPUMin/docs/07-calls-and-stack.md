# 第 07 章：调用、私有栈与 lr

## 目标

第 03 章的"函数"只有一个块：参数进来，值出去，`ret` 就是 `exit`。
本章第一次让函数**回来**，并为此补齐一整套机器级设施：

1. `addi` 成为独立 opcode（`0x06`），带 16 位有符号立即数；
   `add x, 100` 直接选成 `addi`，不再先物化常量；
2. `call` 使用 J 格式（opcode `0x11`），21 位**字**偏移，可达 ±1Mi 字
   （±4MiB）；
3. `ret`（SYS `0x12`，funct=1）返回 lr；kernel 仍以 `exit`（funct=0）
   结束，二者由 `"gpumin_kernel"` 属性区分；
4. ABI 落地：参数 `r4-r11`、返回 `r4-r5`、callee-saved 为
   `r20-r27 + lr`、栈指针 `r30`；
5. 私有栈：prologue 分配、epilogue 回收、lr 的保存/恢复、寄存器 spill
   slot、`FrameIndex` 消除；
6. MC 全链路：21 位 PC-relative fixup、GM32 自己的 relocation 枚举、
   只接受符号的 `call` 操作数、可反汇编、`llc` 的 `.o` 与重汇编的 `.s`
   逐字节一致。

退出条件（可执行）：

```sh
llvm/lib/Target/GPUMin/scripts/check-ch07.sh <llvm-bin-dir>
```

## 背景

### 第 03 章的 `ret` 其实只是 `exit`

第 03 章把 `PseudoRET` 直接展开成 `EXIT`：一个函数执行完，整个 lane
就结束了。这在"一个 kernel 就是一个程序"时没问题，但它不是函数调用：
没有返回地址，没有嵌套，也没有递归。

真正区分两件事的是**谁在等这个返回值**：

- **kernel**：由启动器（第 13 章）派发，lane 结束就是 `exit`；
- **device function**：由另一条 `call` 调用，结束要跳回 lr。

所以本章有两个返回伪指令：`PseudoEXIT`（kernel）和 `PseudoRET`
（device function），选择发生在 `LowerReturn`：

```cpp
unsigned RetOpcode = MF.getFunction().hasFnAttribute("gpumin_kernel")
                         ? GPUMinISD::KRET
                         : GPUMinISD::RET;
```

### lr 是单个寄存器，所以递归必须有栈

`call` 把返回地址写进 `r31`（lr）。考虑 `fib`：

```text
fib(n) -> fib(n-1)          ; 这次 call 覆盖 lr
          fib(n-2)          ; 又覆盖一次
          ret               ; 现在 lr 指向哪里？
```

第一次 `call` 之后，`fib` 自己的返回地址就丢了。唯一的办法是**在进入
函数时把 lr 存到自己的私有栈上，返回前再取回**。这也是本章"私有栈"
存在的第一理由——不是 spill，而是生存。

### 为什么 J 格式按"字"计数

GM32 是 32 位定长指令，所有指令地址的低两位恒为 0。按字节存偏移会浪费
两位；按字存，21 位就能覆盖 ±1,048,576 条指令：

| 位移宽度 | 单位 | 可达范围 |
| --- | --- | --- |
| 16 位（`bra`） | 字 | ±32,768 字 = ±128KiB |
| 21 位（`call`） | 字 | ±1,048,576 字 = ±4MiB |

第 05 章的 `bra` 已经这么做了，`call` 保持一致。代价是编码器/解码器都要
记住"字段是字，不是字节"，这是本章最容易写错的地方之一。

### 为什么 `addi` 是独立 opcode

I 格式把 16 位全给了立即数：

```text
I: [31:26] opcode | [25:21] rd | [20:16] rs1 | [15:0] imm16
```

没有 funct 字段可以区分 add/sub/and/or 的立即数形式。可行的方案有三种：

1. 每种立即数运算一个 opcode（本章的选择，`ADDI = 0x06`）；
2. 挤掉 `rd` 的高位当 funct（会把寄存器堆砍到 16 个）；
3. 只保留 `addi`，其他用 `movi + 寄存器运算`（多一条指令）。

本章选 1：最直白，也给后续 `andi/ori/xori` 留了清晰的编号空间。

### kernel ABI 与 device ABI 是两套入口

kernel 的参数不来自 `r4-r11`，而来自启动描述符（plan §4.5，第 13 章）。
本章只实现 **device function** 的寄存器 ABI；kernel 在本章只是"会调用
device function、并以 `exit` 结束"的入口。

## 设计

### 指令增量

| 指令 | 格式 | 编码 |
| --- | --- | --- |
| `addi rd, rs1, imm` | I | `[31:26]=0x06 \| [25:21]=rd \| [20:16]=rs1 \| [15:0]=imm` |
| `call off` | J | `[31:26]=0x11 \| [25:21]=0 \| [20:0]=off`（字） |
| `ret` | SYS | `[31:26]=0x12 \| [10:0]=1` |
| `exit` | SYS | `[31:26]=0x12 \| [10:0]=0` |

实测编码：

```text
addi r1, r2, -16      -> f0 ff 22 18
addi r31, r30, 1024   -> 00 04 fe 1b
addi r0, r0, 32767    -> ff 7f 00 18
call tgt (tgt 在 +2 字) -> 02 00 00 44
ret                   -> 01 00 00 48
exit                  -> 00 00 00 48
```

### ABI

| 项 | 约定 |
| --- | --- |
| 参数 | `r4-r11`（本章只支持寄存器参数） |
| 返回 | `r4-r5` |
| callee-saved | `r20-r27`，以及 `lr`（`r31`） |
| caller-saved | `r1-r3`、`r12-r19` |
| 栈指针 | `r30`，4 字节对齐 |
| 零寄存器 | `r0` |
| 变参 / 栈参数 | 本章不支持（`report_fatal_error`） |

`lr` 同时是 **reserved** 和 **callee-saved**——这是 AArch64 的模型：

- reserved 保证寄存器分配器永远不会把值放进 lr；
- callee-saved 保证 PEI 会为它分配 spill slot，并把它算进调用保留掩码。

两者缺一不可。只 reserved 不 callee-saved，PEI 的 CSI 根本不会包含它
（CSI 是从 callee-saved 列表里筛出来的），lr 就静默地不被保存。

### 帧布局

```text
        高地址
        +----------------------+
        |   caller 的帧        |
        +----------------------+  <- 进入函数时的 r30
        |   lr slot            |  st.local.b32 r31, 12(r30)
        |   r20 ... r27        |  st.local.b32 r20,  8(r30)
        |   spill / locals     |  st.local.b32 r21,  4(r30)
        +----------------------+  <- prologue 之后的 r30
        低地址
```

- prologue：`addi r30, r30, -N`（N 是 `MFI.getStackSize()`）；
- 所有 slot 用**正偏移** `0(r30) .. N-4(r30)` 访问；
- epilogue：先恢复 callee-saved，再 `addi r30, r30, +N`，最后 `ret`；
- N 必须能用 16 位 `addi` 表示，否则本章直接报错（练习 5）。

### 一次调用的五个阶段

```text
1. LowerCall:  AnalyzeCallOperands -> 参数进 r4..r11
2.            getCopyToReg + DAG.getRegister（让参数 live-in）
3.            GPUMinISD::CALL(chain, tglobaladdr, arg regs,
                              regmask, glue)
4. ISel:      CALL 指令（Defs=[r31]）
5. 被调用者:  prologue 保存自己的 lr；epilogue 恢复后 ret
```

第 3 步里三样东西缺一不可：

- **chain**：调用有副作用，不能和别的内存操作乱序；
- **regmask**：告诉分配器哪些寄存器活着；没有它，机器验证器会报
  `Using an undefined physical register ... implicit $r4`；
- **glue**：保证参数 copy 一定排在 `call` 之前。

### 为什么用 target node 而不是 machine node

在 `LowerCall` 里直接建 `DAG.getMachineNode(GPUMin::CALL, ...)` 会触发
`Node already selected!`。正确的做法是建 **target-specific SDNode**
（`GPUMinISD::CALL`），交给 `GPUMinDAGToDAGISel::Select` 去选。这样
调度器、合法化器和 ISel 都还能看到它。

## 实现

本章改动 23 个已有文件、新增 3 个（`ELFRelocs/GPUMin.def`、
`check-ch07.sh`、本文档），约 550 行。

| 层 | 文件 | 内容 |
| --- | --- | --- |
| TableGen | `GPUMinInstrFormats.td` | `GPUMinJ`：`bits<21> off` |
| TableGen | `GPUMinInstrInfo.td` | `ADDI`/`CALL`/`RET`/`PseudoEXIT`/`PseudoRET`、`SDT_GPUMinCall` |
| CodeGen | `GPUMinISelLowering.cpp` | `LowerCall`、`LowerReturn`、KRET/RET 选择 |
| ABI | `GPUMinCallingConv.td` | `CSR_GPUMin = r31 + r20-r27` |
| CodeGen | `GPUMinRegisterInfo.cpp` | lr reserved、`eliminateFrameIndex`、scavenger |
| CodeGen | `GPUMinInstrInfo.cpp` | spill/restore、分支分析、`copyPhysReg` |
| CodeGen | `GPUMinFrameLowering.cpp` | `determineCalleeSaves`、prologue/epilogue |
| MC | `GPUMinFixupKinds.h` | `fixup_gpumin_21_pcrel` |
| MC | `GPUMinAsmBackend.cpp` | 21 位 fixup 的 apply + 越界诊断 |
| MC | `GPUMinMCCodeEmitter.cpp` | `getCallTargetOpValue` |
| MC | `AsmParser` | `call` 只接受符号 |
| MC | `Disassembler` | `DecodeCallTarget21`（`off * 4`） |
| MC | `GPUMinMCInstLower.cpp` | `MO_GlobalAddress -> MCSymbolRefExpr` |
| ELF | `ELFRelocs/GPUMin.def` | `R_GPUMIN_*` |
| ELF | `GPUMinELFObjectWriter.cpp` | fixup -> relocation 映射 |
| ELF | `Object/ELF.cpp` | relocation 名字打印（objdump/readobj 可读） |

### J 格式与 CALL

```tablegen
class GPUMinJ<bits<6> op, dag outs, dag ins, string asmstr>
    : GPUMinInst<outs, ins, asmstr, []> {
  bits<21> off;
  let Inst{31-26} = op;
  let Inst{25-21} = 0;
  let Inst{20-0}  = off;   // 字偏移，不是字节
}

def CALL : GPUMinJ<OPC_CALL.Value, (ins calltarget21:$off), "call $off"> {
  let isCall = 1;
  let hasSideEffects = 1;
  let Defs = [R31];        // call 写 lr
}
```

AsmString 里的 `$off` 必须和字段同名，否则 TableGen 会报
`could not find field for operand 'off'`。

### 21 位 fixup

`applyFixup` 拿到的 `Value` 已经是 `target - instruction address`（字节）。
先除以 4，再检查范围，最后写三个字节：

```cpp
int64_t Off = static_cast<int64_t>(Value);
if (Off & 3)
  getContext().reportError(Fixup.getLoc(),
                           "GPUMin: branch target is not 4-byte aligned");
Off /= 4;
if (!isInt<21>(Off))
  getContext().reportError(Fixup.getLoc(), "GPUMin: call target out of range");
Data[0] = static_cast<uint8_t>(Off & 0xff);
Data[1] = static_cast<uint8_t>((Off >> 8) & 0xff);
Data[2] = static_cast<uint8_t>((Off >> 16) & 0x1f);
```

### 重定位

目标文件不能只靠"就地打的补丁"：符号可能被链接器移动。GM32 有自己的
machine number，于是也定义自己的重定位编号：

```c
ELF_RELOC(R_GPUMIN_NONE,        0)
ELF_RELOC(R_GPUMIN_32,          1)
ELF_RELOC(R_GPUMIN_32_PCREL,    2)
ELF_RELOC(R_GPUMIN_16_PCREL,    3)
ELF_RELOC(R_GPUMIN_21_PCREL,    4)
ELF_RELOC(R_GPUMIN_RELATIVE,    5)
```

两条路径的结果不同，这是本章最值得观察的现象：

- **本文件内的局部符号**：MC 能算出最终地址，直接写进指令字，不产生
  重定位。反汇编看到 `call -10` 这样的真实字偏移；
- **链接器可见的全局符号**：指令字留 0，另发一条
  `R_GPUMIN_21_PCREL`，由链接器按 `S + A - P` 重算。

### 私有栈

`GPUMinFrameLowering::determineCalleeSaves`：

```cpp
TargetFrameLowering::determineCalleeSaves(MF, SavedRegs, RS);
// 只要有调用，lr 就必须保存：这不是分配器的选择。
if (MF.getFrameInfo().hasCalls())
  SavedRegs.set(GPUMin::R31);
```

`eliminateFrameIndex` 负责把 `FrameIndex` 变成 `sp + offset`：
`objectOffset + StackSize + SPAdj`。偏移超过 11 位时，用
`RegScavenger` 物化一个地址寄存器（本章的帧都很小，主要留给后续章节）。

## 代码走读

输入：

```llvm
target triple = "gpumin-unknown-elf"

define i32 @fib(i32 %n) {
  %c = icmp slt i32 %n, 2
  br i1 %c, label %base, label %rec
base:
  ret i32 %n
rec:
  %n1 = sub i32 %n, 1
  %n2 = sub i32 %n, 2
  %a = call i32 @fib(i32 %n1)
  %b = call i32 @fib(i32 %n2)
  %r = add i32 %a, %b
  ret i32 %r
}
```

`llc -march=gpumin -verify-machineinstrs`：

```asm
fib:
	addi r30, r30, -16        # 私有帧
	st.local.b32 r31, 12(r30) # 保存返回地址（lr）
	st.local.b32 r20, 8(r30)  # callee-saved
	st.local.b32 r21, 4(r30)
	movi r1, 1
	setp.gt p0, r4, r1
	bra p0, .LBB0_2           # n > 1 -> 递归分支
	bra .LBB0_1               # 否则落到 base
.LBB0_2:
	addi r1, r4, -1
	mov r21, r4               # 第一次递归的参数暂存
	mov r4, r1
	call fib                  # 参数在 r4
	mov r20, r4               # 第一次结果暂存
	addi r4, r21, -2          # 第二次递归的参数
	call fib
	add r4, r20, r4
.LBB0_1:
	ld.local.b32 r21, 4(r30)
	ld.local.b32 r20, 8(r30)
	ld.local.b32 r31, 12(r30) # 恢复自己的返回地址
	addi r30, r30, 16
	ret
```

四处细节：

1. `call` 之前参数一定在 `r4`：第二次递归的 `addi r4, r21, -2` 就是证明；
2. `r20/r21` 是 callee-saved：第一次 `call` 的结果放 `r20`，第二次
   `call` 不会把它冲掉；
3. `lr` 在 prologue 保存、在 `ret` 之前恢复——恢复的是 **fib 自己的**
   返回地址，不是调用者的；
4. 局部调用在 `.o` 里是真实偏移；把 `fib` 改成 `internal` 后，同一段
   代码反汇编为 `call -10`（0x48 -> 0x20）。

kernel 的结尾不同：

```asm
kernel:
	addi r30, r30, -16
	st.local.b32 r31, 12(r30)
	ld.global.b32 r4, 0(r4)
	call fib
	ld.local.b32 r31, 12(r30)
	addi r30, r30, 16
	exit                      # kernel 结束整个 lane
```

## 验证

```text
$ llvm/lib/Target/GPUMin/scripts/check-ch07.sh /home/ubuntu/llvm/builds/pr-ctpop/bin
PASS 1/4: ADDI encodes, checks its range and folds small constants
PASS 2/4: CALL is symbolic, word-scaled and range-checked
PASS 3/4: ret/exit split, private frame and lr preservation
PASS 4/4: codegen object and reassembled .s are identical
OK: chapter 07 gate passed
```

四组分别覆盖：

1. **ADDI**：三条实测编码、±32768 边界、`add x, 100 -> addi`；
2. **CALL**：局部目标解析为 `call 2`；`call 3` 被拒；`.space 8388608`
   触发 `call target out of range`；全局目标产生 `R_GPUMIN_21_PCREL`；
3. **ABI**：两个 `ret` + 一个 `exit`；帧分配；lr 存取；三次 `call fib`；
   参数走 `r4`；
4. **对象一致性**：`llc` 的 `.o` 与重汇编 `.s` 的 `objdump -d -r` 输出
   逐行相同。

手工复核：

```sh
llc -march=gpumin /tmp/call.ll -o /tmp/call.s
llvm-mc -triple=gpumin -filetype=obj /tmp/call.s -o /tmp/call-mc.o
llvm-objdump -d -r /tmp/call.o
llvm-readobj -r /tmp/call.o
```

## 常见坑

1. **AsmString 的字段名必须存在**。`call $off` 对应 `bits<21> off`；
   写成 `$dst` 而类里没有 `dst`，TableGen 直接报
   `could not find field for operand 'dst'`。
2. **带 chain 的 SDNode，chain 必须是 operand 0**。否则合法化器报
   `operand #0 has invalid type; expected ch, got i32`。
3. **不要在 lowering 里建 machine node**。`DAG.getMachineNode(CALL, ...)`
   会触发 `Node already selected!`；建 target node（`GPUMinISD::CALL`），
   让 ISel 选。
4. **变参节点的类型 profile** 是 `SDTypeProfile<0, -1, [...]>`；写成
   `<0, 1>` 会报 `requires exactly 1 operands`。
5. **`RegScavenger` 的头文件**是 `llvm/CodeGen/RegisterScavenging.h`，
   不是 `RegScavenger.h`。
6. **本仓库的 `TargetInstrInfo` 与上游签名不同**：
   `insertBranch` 返回 `unsigned`、参数顺序是
   `(MBB, TBB, FBB, Cond, DL, BytesAdded)`；`storeRegToStackSlot` 最后是
   `MIFlag`，`loadRegFromStackSlot` 多一个 `SubReg`。照抄上游代码会编译
   失败。
7. **`BuildMI` 收引用**：`BuildMI(MBB, ...)`，不是 `BuildMI(&MBB, ...)`。
8. **CALL 必须带 regmask**。不调用 `getCallPreservedMask` 时，验证器报
   `Using an undefined physical register ... PseudoRET implicit $r4`。
9. **lr 只 reserved 不够**。PEI 的 CSI 是从 callee-saved 列表里筛出来的
   （`assignCalleeSavedSpillSlots`），lr 不在列表里就永远不会被 spill。
   正确姿势：reserved + callee-saved，两者同时成立。
10. **`analyzeBranch` 里不要调 `MBB.getFallThrough()`**。它内部会回调
    `analyzeBranch`，直接无限递归。条件分支的 false 边用
    `FBB = nullptr` 表示，让布局去推断。
11. **普通 fallthrough 块要返回 `false`（"可分析、无分支"）**，不能返回
    `true`。否则 block placement 报
    `Unexpected block with un-analyzable fallthrough!`。
12. **条件分支不要加 `isBarrier = 1`**。机器验证器会报
    `MBB exits via conditional branch/fall-through but ends with a barrier
    instruction`。`isBarrier` 是给无条件分支/返回用的。
13. **没有 relocation 映射，`.o` 写不出来**。`llc -filetype=obj` 会报
    `GPUMin: unsupported relocation`；需要给每个 fixup 一个
    `R_GPUMIN_*`，并在 `Object/ELF.cpp` 里加名字映射，否则 objdump 只能
    打印 `Unknown`。
14. **全局符号和局部符号的 `.o` 不一样**。前者是 `call 0` + 重定位，
    后者是就地解析的真实偏移。写门禁时要么用局部标签，要么检查
    relocation，不要混为一谈。

## 练习

1. **栈参数**：给 `LowerCall` 分配 outgoing argument area，让 9 个 i32
   参数的函数工作；`AnalyzeCallOperands` 返回非零 `getStackSize()` 时
   不再报错。
2. **间接调用**：给 J 格式加一个寄存器形式（或新 R 格式 `callr rN`），
   让 `LowerCall` 接受非 `GlobalAddressSDNode` 的 callee。
3. **尾调用**：识别 `musttail`，在 epilogue 之后直接 `bra`，不写 lr。
4. **叶子优化**：`determineCalleeSaves` 只在真正需要时保存 lr——现在的
   实现是 `hasCalls()` 就保存，叶子函数本来就不该有帧。
5. **大帧**：`StackSize` 超过 16 位 `addi` 时，用 `movi + add` 或多次
   `addi` 分配，不要 `report_fatal_error`。
6. **最小链接器**：读 `.o` 的 `R_GPUMIN_21_PCREL`，把 `S + A - P` 写回
   指令字，再跑 `llvm-objdump` 验证。
7. **varargs**：`LowerFormalArguments` / `LowerCall` 支持
   `CallingConv::C` 的可变参数（提示：需要一个溢出区指针）。

## 下一章

第 08 章：**类型合法化**。本章的 ABI 只处理 i32；下一章把 i1/i8/i16/i64
接进来：小整数怎么提升、i64 怎么拆成两个 i32、窄 load/store 怎么扩展，
以及小端布局在寄存器对上的顺序。调用与栈是它们的载体——没有本章的
frame，i64 的寄存器对连溢出都做不到。
