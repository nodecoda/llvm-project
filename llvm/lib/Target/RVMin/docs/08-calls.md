# 第 8 章：函数调用、递归与返回地址

## 目标

第 7 章之前，每个函数都是孤岛：`ra` 被保留，谁也不能调用谁。本章接上控制流
图中最重要的边——`call`：

```c
int fib(int n) {                 /* 递归 */
  if (n < 2) return n;
  return fib(n - 1) + fib(n - 2);
}

int sum10(int a, int b, int c, int d, int e,
          int f, int g, int h, int i, int j) {  /* 栈参数 */
  return a + b + c + d + e + f + g + h + i + j;
}

int apply(int (*f)(int), int x) { return f(x); }    /* 间接调用 */
extern int ext(int);
int call_ext(int x) { return ext(x) + 1; }          /* 外部符号 */
```

完成本章后：

1. `i32` 参数按 ILP32 走 `a0-a7`，第 9 个起放栈上；
2. 返回值在 `a0`（需要时 `a1`）；
3. 非叶子函数自动保存/恢复 `ra`，叶子函数不付这笔开销；
4. callee-saved（`s0-s11`）在调用点被正确建模；
5. 直接调用、间接调用、外部符号调用都能生成对象文件；
6. 递归与相互递归正确（`fact`、`is_even`/`is_odd`）。

## 代码地图

| 文件 | 本章改动 |
| --- | --- |
| `RVMinCallingConv.td` | 新增 `CSR_RVMin`：`ra` + `s0-s11` 的 callee-saved 集合 |
| `RVMinRegisterInfo.{h,cpp}` | 释放 `ra`；`getCallPreservedMask` 返回 `CSR_RVMin_RegMask` |
| `RVMinInstrInfo.td` | `RVMinISD::CALL`、`callseq_start/end`、`PseudoCALL`、`PseudoCALLR` |
| `RVMinISelLowering.{h,cpp}` | `LowerCall`、`LowerFormalArguments` 的栈参数 |
| `RVMinAsmPrinter.cpp` | `PseudoCALL` → `LUI + ADDI + JALR` 展开 |
| `RVMinFrameLowering.{h,cpp}` | callee-saved 的 spill/restore；删除 `ADJCALLSTACK*` |
| `scripts/rvmin-global-corpus.s` | 调用序列的字节门禁 |
| `llvm/test/CodeGen/RVMin/calls.ll` | 本章 FileCheck（含 `-O0`、重定位） |

## 关键实现

### 1. ILP32 的一页纸约定

| 项目 | 约定 |
| --- | --- |
| 整型参数 | `a0-a7`（`x10-x17`），超出部分右到左压栈 |
| 栈参数 | 4 字节对齐，位于调用者 `sp + 0, +4, ...` |
| 整型返回值 | `a0`，`i64` 用 `a0:a1`（第 9 章） |
| caller-saved | `ra`、`t0-t6`、`a0-a7` |
| callee-saved | `s0-s11`（`x8,x9,x18-x27`） |
| 调用点 `sp` 对齐 | 16 字节 |

调用约定仍然只写在 `RVMinCallingConv.td` 一个地方：

```tablegen
def CC_RVMin : CallingConv<[
  CCIfType<[i32], CCAssignToReg<[X10, X11, X12, X13, X14, X15, X16, X17]>>,
  CCIfType<[i32], CCAssignToStack<4, 4>>
]>;

def CSR_RVMin : CalleeSavedRegs<(add X1, X8, X9, (sequence "X%u", 18, 27))>;
```

`CSR_RVMin` 一份声明、两个消费者：

- **PEI**：函数体里修改过的 callee-saved 寄存器要在这里保存/恢复
  （`CSR_RVMin_SaveList`）；
- **寄存器分配器**：调用点能“活下来”的寄存器集合
  （`CSR_RVMin_RegMask`），其余寄存器在调用后一律视为被破坏。

`TargetRegisterInfo::getCallPreservedMask` 的默认返回值是 `nullptr`，语义是
**什么都不能跨调用存活**。第 7 章之前没有调用，没人踩到；本章必须显式返回
`CSR_RVMin_RegMask`，否则所有活跃值都会被保守地溢出。

### 2. `ra` 的一生

第 6/7 章把 `ra` 留在 `getReservedRegs()` 里，因为没人会写它。本章的调用指令
带 `Defs = [X1]`：一次调用会把 `ra` 写成返回地址。于是：

```text
caller:
    addi sp, sp, -16
    sw   ra, 12(sp)     # 序言：保存自己的返回地址
    ...
    jalr ra, 0(t0)      # 调用：ra ← 返回地址
    ...
    lw   ra, 12(sp)     # 收尾：恢复自己的返回地址
    addi sp, sp, 16
    jalr zero, 0(ra)    # 返回
```

这不是魔法，是 PEI 的标准流程：

1. `determineCalleeSaves` 扫描函数，发现 `X1` 被“修改”过（调用指令的隐式
   def），把 `X1` 标进 `PrologCSRs`；
2. `calculateCalleeSavedRegisters` 为它分配一个栈槽；
3. `spillCalleeSavedRegisters` / `restoreCalleeSavedRegisters` 由目标实现，
   在序言/收尾各发一条 `sw`/`lw`。

叶子函数没有任何指令写 `ra`，所以 PEI 不会保存它——`fib` 的基线分支、`callee`
都只有一条 `jalr zero, 0(ra)`。

> 第 6 章实现了 `storeRegToStackSlot`/`loadRegFromStackSlot`，本章的
> spill/restore 直接复用，没有新的栈操作代码。

### 3. 调用在 DAG 里的形状

`LowerCall` 把一次调用展开成一条链：

```text
CALLSEQ_START <栈参数大小>          → ADJCALLSTACKDOWN
  sw  <第 9、10… 个参数>, 0(sp)/4(sp)
  CopyToReg a0..a7 <- 参数
  RVMinISD::CALL chain, callee, a0..a7, regmask
CALLSEQ_END                         → ADJCALLSTACKUP
  CopyFromReg a0 <- 返回值
```

`RVMinISD::CALL` 的变长操作数是本章最容易踩的坑。它的定义必须带
`SDNPVariadic`：

```tablegen
def RVMinCall : SDNode<"RVMinISD::CALL", SDT_RVMinCall,
                       [SDNPHasChain, SDNPOutGlue, SDNPOptInGlue,
                        SDNPVariadic]>;
```

否则 TableGen 生成的匹配器只把**模式里写过**的操作数搬到机器指令上，
`a0-a7` 的寄存器操作数和 `regmask` 会被静默丢掉——表现为“参数寄存器没赋值”、
“调用点没有 clobber 信息”。带上 `SDNPVariadic` 后生成的代码是
`OPFL_Variadic1`，多余操作数会原样附到 `PseudoCALL` 上：

```text
PseudoCALL @many, csr_rvmin, implicit-def dead $x1,
           implicit $x10, ..., implicit $x17
```

`callseq_start` / `callseq_end` 也不是通用节点：每个 target 要自己用
`SDCallSeqStart` / `SDCallSeqEnd` 定义一次，并挂到
`ADJCALLSTACKDOWN`/`ADJCALLSTACKUP` 的 pattern 上：

```tablegen
def callseq_start : SDNode<"ISD::CALLSEQ_START", SDT_RVMinCallSeqStart,
                           [SDNPHasChain, SDNPOutGlue]>;
def callseq_end : SDNode<"ISD::CALLSEQ_END", SDT_RVMinCallSeqEnd,
                         [SDNPHasChain, SDNPOptInGlue, SDNPOutGlue]>;

let Defs = [X2], Uses = [X2] in {
def ADJCALLSTACKDOWN : RVMinPseudo<(outs), (ins i32imm:$amt1, i32imm:$amt2), "",
                                   [(callseq_start timm:$amt1, timm:$amt2)]>;
def ADJCALLSTACKUP : RVMinPseudo<(outs), (ins i32imm:$amt1, i32imm:$amt2), "",
                                 [(callseq_end timm:$amt1, timm:$amt2)]>;
}
```

### 4. 栈参数：调用者存，被调用者读

RVMin 没有帧指针，`hasReservedCallFrame()` 为真：PEI 把**最大的栈参数区**
一次性算进固定栈帧，`sp` 在调用点不动。因此：

- `ADJCALLSTACKDOWN/UP` 只是占位符，`eliminateCallFramePseudoInstr` 直接删掉；
- 调用者把第 9、10 个参数写到 `sp+0`、`sp+4`，两条 `sw` 的偏移由
  `(store val, (add sp, 常量))` 模式折叠进 S 型立即数；
- 被调用者用 `CreateFixedObject(4, offset, /*IsImmutable=*/true)` 把入口栈上的
  参数建模成固定栈对象，第 6 章的 frame-index 机制把它变成一条 `lw`。

`caller` 的反汇编（`-O1`）：

```text
call_many:
	addi sp, sp, -16        # 8 字节参数区，向上取整到 16
	sw   ra, 12(sp)
	addi a0, zero, 10
	sw   a0, 4(sp)          # 第 10 个参数
	addi a0, zero, 9
	sw   a0, 0(sp)          # 第 9 个参数
	addi a0, zero, 1
	...
	lui  t0, %hi(many)
	addi t0, t0, %lo(many)
	jalr ra, 0(t0)
```

被调用者：

```text
many:
	add a0, a0, a1
	...                     # a0-a7
	lw  a1, 0(sp)           # 第 9 个参数
	add a0, a0, a1
	lw  a1, 4(sp)           # 第 10 个参数
	add a0, a0, a1
	jalr zero, 0(ra)
```

### 5. 直接调用与间接调用

**直接调用**用一个 code-gen only 的伪指令携带符号：

```tablegen
let isCall = 1, Defs = [X1], Size = 12 in
def PseudoCALL : RVMinPseudo<(outs), (ins bare_symbol:$func), "", []>;

def : Pat<(RVMinCall tglobaladdr:$func), (PseudoCALL tglobaladdr:$func)>;
def : Pat<(RVMinCall texternalsym:$func), (PseudoCALL texternalsym:$func)>;
```

它在 `RVMinAsmPrinter::emitPseudoCall` 里展开成三条真实指令：

```text
lui  t0, %hi(callee)
addi t0, t0, %lo(callee)
jalr ra, 0(t0)
```

为什么不直接用 `auipc` + `jalr`？真实 RISC-V 的 `call` 伪指令是
`AUIPC + JALR` 加 `R_RISCV_CALL`，链接器可以把这对指令松弛成一条 `jal`。
那需要 `R_RISCV_RELAX` 和链接器松弛支持；本章选择复用第 7 章已经验证过的
`%hi/%lo` 绝对地址对，代价是 12 字节而不是 8 字节，好处是**不需要新增任何
MC/重定位代码**，对象文件也能被现有工具链直接检查。

**间接调用**没有符号，地址已经在寄存器里，一条 `jalr ra, rs1, 0` 就够：

```tablegen
let isCall = 1, Defs = [X1], Size = 4 in
def PseudoCALLR : RVMinPseudo<(outs), (ins GPR:$rs1), "", []>,
                  PseudoInstExpansion<(JALR X1, GPR:$rs1, 0)>;

def : Pat<(RVMinCall GPR:$rs1), (PseudoCALLR GPR:$rs1)>;
```

这次能直接用 TableGen 的 `PseudoInstExpansion`，因为展开结果只有一条指令。

### 6. 重定位

直接调用复用第 7 章的两个重定位：

```text
$ llvm-readelf -r calls.o
R_RISCV_HI20    ext + 0
R_RISCV_LO12_I  ext + 0
```

同一节内的函数（`fib` 调用自己、`is_even` 调用 `is_odd`）也会生成重定位，
因为重定位对象文件里 `.text` 的最终地址尚未确定。

## 验证

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop; B=/home/ubuntu/llvm/builds/pr-ctpop
ninja -C "$B" -j24 llc clang llvm-mc llvm-objdump llvm-readelf llvm-objcopy FileCheck

"$B/bin/llvm-lit" -sv "$W/llvm/test/CodeGen/RVMin"     # 6 tests
"$W/llvm/lib/Target/RVMin/scripts/check-rvmin-encodings.sh" "$B/bin"
```

逐字节复核（`llc -S` 的输出重新汇编，与 `llc -filetype=obj` 对比）：

```bash
"$B/bin/llc" -march=rvmin -mtriple=riscv32-unknown-elf -O1 calls.ll \
  -filetype=obj -o a.o
"$B/bin/llc" -march=rvmin -mtriple=riscv32-unknown-elf -O1 calls.ll -o a.s
"$B/bin/llvm-mc" -triple=rvmin -filetype=obj a.s -o b.o
"$B/bin/llvm-objcopy" -O binary --only-section=.text a.o a.bin
"$B/bin/llvm-objcopy" -O binary --only-section=.text b.o b.bin
cmp a.bin b.bin
```

clang 端到端（递归、10 参数、函数指针、外部符号）：

```bash
"$B/bin/clang" --target=riscv32-unknown-elf -march=rv32i -mabi=ilp32 \
  -O1 -S -emit-llvm calls.c -o calls.ll
"$B/bin/llc" -march=rvmin -mtriple=riscv32-unknown-elf -O1 calls.ll \
  -filetype=obj -o calls.o
"$B/bin/llvm-objdump" -d --triple=riscv32 calls.o
```

`fib` 的 `-O1` 结果（`s0-s2` 与 `ra` 一起保存，尾块统一恢复）：

```text
fib:
	addi sp, sp, -16
	sw   ra, 12(sp)
	sw   s0, 8(sp)
	sw   s1, 4(sp)
	sw   s2, 0(sp)
	...
	lui  t0, %hi(fib)
	addi t0, t0, %lo(fib)
	jalr ra, 0(t0)
	...
	lw   ra, 12(sp)
	lw   s0, 8(sp)
	lw   s1, 4(sp)
	lw   s2, 0(sp)
	addi sp, sp, 16
	jalr zero, 0(ra)
```

## 当前限制（第 8 章）

- 没有尾调用（`IsTailCall` 强制为 false）：`return f(x);` 先 `call` 再返回。
  尾调用需要复用调用者的栈帧，属于第 13 章的优化。
- 没有 `byval`、`sret`、结构体按值传参；它们依赖第 9 章的类型合法化。
- 变参函数仍然显式报错（`varargs are not supported yet`）。
- 直接调用是 `LUI+ADDI+JALR`（12 字节）而不是 `AUIPC+JALR + R_RISCV_CALL`
  （8 字节，可被链接器松弛成 4 字节）。没有链接器松弛，所以也没有
  `R_RISCV_CALL_PLT`。
- 调用点没有任何调度/优化：`PseudoCALL` 的 `Size` 固定为 12，没有
  `.p2align` 或 branch relaxation。
- 帧内偏移仍限制在 `[-2048, 2047]`，因此栈参数区也不能超过约 2 KiB。
