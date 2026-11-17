# 第 44 章：压缩代码生成

## 目标

第 43 章让 35 条 `c.*` 指令可以被汇编、编码、反汇编，但 `llc` 一条也不会
生成——它们只能手写。本章把“32 位指令 → 16 位指令”的投影接进代码生成，
完成之后：

1. `-mattr=+c` 下 `addi/add/lw/sw/lui/jal/jalr/beq/bne/ebreak` 等指令在
   操作数允许时自动变成 `c.addi/c.add/c.lw/c.sw/c.lui/c.j/c.jalr/c.beqz/...`；
2. 压缩规则由 `CompressPat` 表驱动，运行时由 TableGen 生成的
   `compressInst()` 逐条校验寄存器类与立即数范围，不靠手写 if；
3. `-march=rv32ic` 的程序在 QEMU 下运行结果与 RV32I 完全一致；
4. 同一份 IR、同一 `-O` 级，`+c` 的 `.text` **严格小于**不开 C 的版本
   （本章实测 demo：784 → 592 字节）；
5. 压缩后的分支/跳转仍由第 43 章的 `rvc_branch/rvc_jump` fixup 解析，
   标签偏移正确；
6. 不开 `+c` 时输出与此前逐字节一致（压缩只在 AsmPrinter 里发生）。

本章不做：linker relaxation、`getInstSizeInBytes` 的精确压缩尺寸、
汇编器对任意 32 位助记符的自动压缩（显式 `c.*` 已在第 43 章支持）。

## 背景：压缩是投影，不是另一套代码生成

第 43 章已经把完整的 16 位编码模型放进了 TableGen：每条 `c.*` 有
`bits<16> Inst`、`Size = 2`、自己的立即数位序。于是“压缩”不需要新的
SelectionDAG 节点、不需要新的寄存器分配，只需要回答一个问题：

> 这条已经选好的 32 位指令，它的操作数是否落在某个 16 位编码的域里？

这正是 `-gen-compress-inst-emitter` 后端的职责。它读取 `CompressPat`：

```tablegen
def : CompressPat<(ADDI GPRNoX0:$rs1, GPRNoX0:$rs1, simm6nonzero:$imm),
                  (C_ADDI GPRNoX0:$rs1, simm6nonzero:$imm)>;
```

左边是**展开形式**的指令与操作数约束，右边是**压缩形式**。后端把每条
pattern 变成一个 `case`：

```
switch (MI.getOpcode()) {
case RVMin::ADDI:
  if (MI.getOperand(1).isReg() &&
      getRVMinMCRegisterClass(RVMin::GPRNoX0RegClassID)
          .contains(MI.getOperand(1).getReg()) &&
      RVMinValidateMCOperandForCompress(MI.getOperand(2), STI, ...))
    OutInst.setOpcode(RVMin::C_ADDI), ...
}
```

这里没有任何手写编码知识：位序仍然只在第 43 章的 `.td` 里出现一次。

三个设计后果：

| 选择 | 后果 |
| --- | --- |
| 在 **AsmPrinter** 压缩，而不是 DAG/ISel | `.text` 布局由 MC 层按实际发射字节计算，分支偏移由 fixup 解决；不需要 MachineFunction 级 relaxation |
| 压缩后指令的 `Size = 2` | `llvm-objdump`、`llvm-mc -disassemble`、链接器看到的都是真实的 2 字节流 |
| `getInstSizeInBytes()` 仍返回 4 | MachineFunction 层的大小估计偏保守；不影响正确性，精确实现留作练习 |

## 代码地图

| 文件 | 本章改动 |
| --- | --- |
| `llvm/lib/Target/RVMin/CMakeLists.txt` | 新增 `RVMinGenCompressInstEmitter.inc` |
| `RVMinInstrInfoC.td` | 3 个新操作数、`c_lui_imm`、30+ 条 `CompressPat` |
| `RVMinRegisterInfo.td` | `GPRNoX0`、`GPRNoX0X2`（仅用于 CompressPat 的寄存器子集） |
| `RVMinAsmPrinter.cpp` | `emitMCInst()` 统一发射入口；`compressInst()` 钩子 |
| `AsmParser/RVMinAsmParser.cpp` | `isCLUIImm`（宽形式立即数） |
| `Disassembler/RVMinDisassembler.cpp` | `decodeCLUIImmOperand`（6 位 → 20 位） |
| `llvm/test/CodeGen/RVMin/c-ext.ll` | **新增**：压缩/非压缩的取舍 |
| `scripts/rvmin-e2e.sh` | 6 个特性集、288 对象 |
| `scripts/rvmin-clang-matrix.sh` | `rv32ic` / `rv32imafdc`、288 对象、C 可见性断言 |
| `scripts/rvmin-run.sh` | 压缩 demo 的 QEMU 运行与 `.text` 收缩断言 |
| `scripts/rvmin-verify-all.sh` | 门禁 6 加 `+c`、门禁 8 覆盖压缩 demo |

## 关键实现

### 1. 打开后端并接进构建

```cmake
llvm_tablegen(RVMinGenCompressInstEmitter.inc -gen-compress-inst-emitter)
```

生成的 `.inc` 同时包含三个函数体，用宏选择：

- `GEN_COMPRESS_INSTR` → `compressInst`（本章使用）
- `GEN_UNCOMPRESS_INSTR` → `uncompressInst`（打印器把 `c.*` 展开时用）
- `GEN_CHECK_COMPRESS_INSTR` → `getCompressedSize`（精确尺寸用）

本章只在 `RVMinAsmPrinter.cpp` 里打开 `GEN_COMPRESS_INSTR`；另外两个宏的
代码仍会被 TableGen 生成，所以每个 pattern 操作数都必须同时提供
`MCOperandPredicate` 和 `ImmediateCode`（见“常见坑”）。

### 2. 操作数类型要“比指令更窄”

32 位指令的寄存器操作数是 `GPR`（x0–x31）、`FPR32/FPR64`。压缩编码只能
表达其中一部分，所以 pattern 里用更窄的类型：

| pattern 类型 | 允许的寄存器 | 用于 |
| --- | --- | --- |
| `GPRC` | x8–x15 | `c.lw/c.sw/c.add/c.lui` 等 3 位字段 |
| `SP` | x2 | `c.lwsp/c.swsp/c.addi4spn/c.addi16sp` |
| `GPRNoX0` | x1–x31 | `c.addi/c.li/c.mv/c.jr/c.jalr` |
| `GPRNoX0X2` | x1, x3–x31 | `c.lui`（x2 被 `c.addi16sp` 占用） |
| `FPR32C` / `FPR64C` | f8–f15 | `c.flw/c.fsw/c.fld/c.fsd` |

`GPRNoX0`/`GPRNoX0X2` 在 `RVMinRegisterInfo.td` 里定义为 **GPR 的寄存器
子集**：

```tablegen
def GPRNoX0   : RegisterClass<"RVMin", [i32], 32, (sub GPR, X0)>;
def GPRNoX0X2 : RegisterClass<"RVMin", [i32], 32, (sub GPR, X0, X2)>;
```

CompressInstEmitter 用 `hasSubClass` 做集合包含检查，所以它们不必继承
`GPR` 的定义，只要寄存器集合是子集即可；生成的运行时代码会调用
`getRVMinMCRegisterClass(...).contains(reg)`。

立即数同理：pattern 里用 `simm6nonzero`、`uimm5nonzero`、`uimm7_lsb00`、
`simm9_lsb0` 等，谓词在压缩时重新执行一次。例如 `ADDI rd, rs, 0` 不会被
`C_ADDI` 吃掉（`simm6nonzero` 排除 0），而是落到后面的 `C_MV`。

### 3. pattern 的顺序就是 if 链的顺序

后端按声明顺序为同一个源 opcode 生成有序的 `if` 分支。因此：

- `ADDI x0,x0,0`（`C_NOP`）必须在 `ADDI GPRNoX0...` 之前；
- `ADDI rd,sp,imm`（`C_ADDI4SPN`）先于 `C_ADDI`，否则 x8–x15 + sp 的
  组合会被更宽的规则抢走；
- `ADDI rd,rs,0`（`C_MV`）放在 `C_ADDI` 之后即可，因为非零约束已经互斥。

### 4. `c.lui` 的立即数是宽形式（对第 43 章的修正）

第 43 章把 `c.lui` 的汇编操作数写成 `simm6`，能汇编 `c.lui a0, 1`，但
压缩时暴露了一个真实问题：`lui a0, 1048575` 在 MachineOperand 里是
**符号扩展后的 -1**，而 `simm6` 的解析/打印只认 -32..31，打印成
`c.lui a0, -1` 与规范写法不一致。

本章把操作数改成 `c_lui_imm`：

- 取值范围：`1..31` 或 `0xfffe0..0xfffff`（6 位立即数符号扩展后再左移 12）；
- `PrintMethod = printUImm20Operand`：统一打印 20 位字段（-1 → 1048575）；
- `decodeCLUIImmOperand`：反汇编时把 6 位字段还原成 20 位宽形式；
- `MCOperandPredicate` 同时接受宽形式与 DAG 传来的负值。

这样 `lui` 的压缩判定、汇编输出、反汇编输出三者一致，且
`llvm-mc -triple=riscv32 -mattr=+c` 也能接受我们的输出。

### 5. 统一发射入口

`RVMinAsmPrinter` 里所有发射路径都收敛到一个小函数：

```cpp
void emitMCInst(const MCInst &Inst) {
  MCInst CInst;
  if (compressInst(CInst, Inst, getSubtargetInfo()))
    OutStreamer->emitInstruction(CInst, getSubtargetInfo());
  else
    OutStreamer->emitInstruction(Inst, getSubtargetInfo());
}
```

包括：

- 普通指令（`LowerRVMinMachineInstrToMCInst` 之后）；
- `PseudoRET/PseudoBR/PseudoCALLR` 的 TableGen 展开；
- `PseudoCALL/PseudoTAIL` 手写的 `lui + addi + jalr` 序列。

最后一条很重要：直接调用会展开成 `lui t0, %hi(f); addi t0, t0, %lo(f);
jalr ra, 0(t0)`。统一入口后，`jalr ra, 0(t0)` 会变成 `c.jalr t0`，
`lui/addi` 也可能压缩，函数调用的 `.text` 与不开 C 时同样受益。

### 6. 布局与 fixup

压缩发生在 `MCStreamer::emitInstruction` 之前，因此：

- 段的当前偏移按 2 或 4 字节递增；
- 分支/跳转在解析标签时看到的是**压缩后**的地址差；
- `c.beqz/c.bnez/c.j/c.jal` 走 `fixup_rvmin_rvc_branch`/`_rvc_jump`，
  越界与奇地址报错；
- 4 字节对齐的段尾由 `writeNopData` 补 `c.nop`（第 43 章），压缩后不会
  出现“半个 32 位 NOP”的错误填充。

## 构建与验证

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop

ninja -C "$B" llc clang llvm-mc llvm-objdump

# 1) 本章 lit：压缩取舍 + 重汇编
"$B/bin/llvm-lit" -v "$W/llvm/test/CodeGen/RVMin/c-ext.ll"

# 2) 看一眼典型输出
"$B/bin/llc" -march=rvmin -mtriple=riscv32-unknown-elf -O2 -mattr=+c \
  "$W/llvm/lib/Target/RVMin/scripts/e2e/arith.c" -o - 2>/dev/null || true

# 3) QEMU：一致性 + .text 收缩
bash "$W/llvm/lib/Target/RVMin/scripts/rvmin-run.sh" "$B/bin"

# 4) 288 对象矩阵（e2e 与 clang）
bash "$W/llvm/lib/Target/RVMin/scripts/rvmin-e2e.sh" "$B/bin"
bash "$W/llvm/lib/Target/RVMin/scripts/rvmin-clang-matrix.sh" "$B/bin"

# 5) 8 项门禁
bash "$W/llvm/lib/Target/RVMin/scripts/rvmin-verify-all.sh" "$B/bin"
```

## 预期输出

`llc -mattr=+c` 对 `inc/sum/load/store/konst/big` 的输出：

```asm
inc:
	c.addi a0, 5
	c.jr ra
sum:
	c.add a0, a1
	c.jr ra
load:
	c.lw a0, 0(a0)
	c.jr ra
store:
	c.sw a1, 0(a0)
	c.jr ra
konst:
	c.li a0, 7
	c.jr ra
big:
	c.lui a0, 1
	c.mv a0, a0
	c.jr ra
```

门禁输出：

```
PASS: compression shrank demo .text from 784 to 592 bytes
PASS: 12 programs x 0 1 2 3 optimization levels x 6 feature sets
      = 288 objects, every .text identical to its llvm-mc reassembly
ALL CLANG MATRIX PASS: 288 objects, ELF32 EM_RISCV, -S/-c agree, ...
```

## 常见坑

1. **TableGen 会生成“检查压缩尺寸”的那份代码，即使你只定义
   `GEN_COMPRESS_INSTR`。** 因此每个出现在 pattern 里的操作数都必须同时
   有 `MCOperandPredicate` 和 `ImmediateCode`，否则报
   `does not have a field named 'ImmediateCode'`。第 43 章的 `simm6` 等
   操作数为此补了 `ImmLeaf<i32, ...>`。
2. **窄寄存器类必须是 `RegisterClass`，不能是 `Operand`。** 生成代码走
   `getAsRegClassLike`，只有 RegisterClass 才会生成 `.contains(reg)`
   检查；写成 `Operand` 会被当成立即数分支。
3. **目的 dag 的操作数名字必须出现在源 dag 里。** 否则生成器报
   `Output Dag operand '$rd' has no matching input Dag operand`。像
   `C_ADDI16SP` 这种 rd 固定为 x2 的指令，目的 dag 直接写 `X2` 而不是
   命名寄存器。
4. **`lui` 的 -1 与 1048575 是同一个字段。** 谓词、打印、反汇编三处必须
   统一到宽形式，否则 `-march=rv32ic` 的汇编输出无法再被汇编器接受。
5. **`llvm-objdump` 默认打印别名。** `c.mv` 会显示成 `mv`、`c.swsp` 显示
   成 `sw`，所以门禁检测“是否压缩”要看原始字节列（4 位 hex = 16 位），
   不能用助记符。
6. **`getInstSizeInBytes()` 仍是 4。** 这会让 MachineFunction 层的代码
   尺寸估计偏大；不影响 MC 布局、分支偏移或运行结果，但不要用它做压缩
   收益的统计。
7. **不要在 DAG/ISel 里提前压缩。** 那样分支偏移、对齐、fixup 都要在
   MachineFunction 层重算，等于自己实现一遍 relaxation；本章的投影点
   选在 AsmPrinter 正是为了避开这件事。

## 练习

1. 实现 `getCompressedSize()`（`GEN_CHECK_COMPRESS_INSTR`），让
   `RVMinInstrInfo::getInstSizeInBytes()` 返回真实尺寸，并测量
   `-verify-machineinstrs` 与代码布局是否受影响。
2. 用 `GEN_UNCOMPRESS_INSTR` 让汇编器在 `-mno-c` 时把显式 `c.*` 展开成
   32 位形式（提示：这是上游 `uncompressInst` 的用途）。
3. 增加 `c.addi4spn` 之外的 SP 组合测试：构造 `x8..x15 + sp + 非零 4 字节
   对齐` 的 IR，确认只有 `C_ADDI4SPN` 命中，且 `c.addi16sp` 不被误选。
4. 给 `c.li` 加“立即数为 0 时不压缩”的约束（`c.li rd, 0` 是 HINT），
   观察 `.text` 变化并解释为什么规范允许但不必生成。
5. 统计 `scripts/e2e/*.c` 在 `+c` 下的压缩率，找出收益最低的函数，分析
   是寄存器不在 x8–x15 还是立即数越界。

## 产物清单

- 代码：`CMakeLists.txt`、`RVMinInstrInfoC.td`、`RVMinRegisterInfo.td`、
  `RVMinAsmPrinter.cpp`、`AsmParser/RVMinAsmParser.cpp`、
  `Disassembler/RVMinDisassembler.cpp`。
- 测试：`llvm/test/CodeGen/RVMin/c-ext.ll`。
- 门禁：`rvmin-e2e.sh`、`rvmin-clang-matrix.sh`、`rvmin-run.sh`、
  `rvmin-verify-all.sh`。
- tag：`rvmin-ch44`。
