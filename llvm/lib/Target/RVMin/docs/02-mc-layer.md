# 第 2 章：MC 层、汇编输出与编码正确性

## 目标

第 1 章把 RV32I 写进了 TableGen，但还没有任何“把指令变成字节”的代码。
本章补齐 MC 层，并建立一条**不依赖指令选择**的验证链路：

```text
汇编文本 --(RVMinAsmParser)--> MCInst --(RVMinMCCodeEmitter)--> 32-bit word
                                              |
                                              +--(RVMinAsmBackend)--> fixup/reloc
                                              |
                                              +--(RVMinELFObjectWriter)--> ELF32 EM_RISCV
```

完成本章后：

1. `llvm-mc -triple=rvmin -show-encoding` 能编码本章实现的每条指令；
2. 编码与 `llvm-mc -triple=riscv32` **逐字节一致**；
3. `llvm-mc -triple=rvmin -filetype=obj` 产出合法 ELF32，`e_machine = RISC-V`；
4. 分支/跳转的标签由 fixup 正确回填。

> 为什么不直接测 `llc -filetype=asm`？
> 指令选择第 3 章才有，`llc` 现在还没有 MachineInstr 可打印。
> 因此本章用 `llvm-mc` 作为 MC 层的测试驱动；`llc` 的 asm/obj 门禁移到第 3 章。
> 这也是 LLVM 开发 MC 层的常规做法：先把汇编器/编码器打通，再让 codegen 接进来。

## 为什么本章顺手写了 AsmParser

计划里第 2 章原本只列 InstPrinter / MCCodeEmitter / AsmBackend /
ELFObjectWriter。实际实现时补了一个**最小 AsmParser**，原因有三：

1. 没有汇编器，就无法在指令选择存在之前独立验证编码器；
2. `llvm-mc` 可以直接把 RVMin 的输出和 RISCV 参考编码做上千次对比；
3. 后面每章调试 codegen 时，都能用 `llvm-mc -triple=rvmin` 手工构造指令做对照。

AsmParser 只认识三种操作数：寄存器、表达式、`imm(reg)`；指令匹配全部由
TableGen 生成的 matcher 完成（`-gen-asm-matcher`）。

## 代码地图

| 文件 | 作用 |
| --- | --- |
| `MCTargetDesc/RVMinInstPrinter.{h,cpp}` | MCInst → 汇编文本；打印 ABI 寄存器名 |
| `MCTargetDesc/RVMinMCCodeEmitter.{h,cpp}` | 操作数取值 + 符号 fixup；写 4 字节小端 |
| `MCTargetDesc/RVMinAsmBackend.{h,cpp}` | B/J 立即数回填、NOP 数据、通用 fixup |
| `MCTargetDesc/RVMinELFObjectWriter.{h,cpp}` | `ELF32`, `EM_RISCV`, RELA |
| `MCTargetDesc/RVMinFixupKinds.h` | `fixup_rvmin_branch/jal/32` |
| `AsmParser/RVMinAsmParser.cpp` | 寄存器/表达式/内存操作数、`.word/.half` |
| `RVMinMCInstLower.{h,cpp}` | MachineInstr → MCInst（第 3 章开始使用） |
| `RVMinAsmPrinter.cpp` | `emitInstruction` 走 MC 层 |

TableGen 新增三个输出（`RVMin/CMakeLists.txt`）：

```cmake
llvm_tablegen(RVMinGenAsmMatcher.inc -gen-asm-matcher)
llvm_tablegen(RVMinGenAsmWriter.inc -gen-asm-writer)
llvm_tablegen(RVMinGenMCCodeEmitter.inc -gen-emitter)
```

## 编码路径逐段看

### 1. 操作数取值

生成的 `getBinaryCodeForInstr` 对每个操作数调用
`RVMinMCCodeEmitter::getMachineOpValue`：

- 寄存器 → `MCRegisterInfo::getEncodingValue`（即 `x0`–`x31` 的 5 bit 编号）；
- 立即数 → 原值，由 TableGen 的 `Inst{...}` 布局摆放；
- 符号表达式 → 记录 `MCFixup` 并返回 0。

```cpp
case RVMin::BEQ: case RVMin::BNE: case RVMin::BLT: case RVMin::BGE:
  Kind = rvmin::fixup_rvmin_branch; break;
case RVMin::JAL:
  Kind = rvmin::fixup_rvmin_jal; break;
```

注意：本树（LLVM 24）的 `MCFixup::create` **没有 SMLoc 参数**，PC-relative
是 fixup 自身的属性：

```cpp
MCFixup::create(0, Expr, Kind, /*PCRel=*/true)
```

`MCFixupKindInfo::Flags` 里也已经没有 `FKF_IsPCRel`；`MCAssembler` 直接读
`Fixup.isPCRel()`。

### 2. 立即数回填

分支和 JAL 的立即数不是连续位段，`applyFixup` 必须按 ISA 的拆位公式写：

```text
B: imm[12]->31, imm[10:5]->30:25, imm[4:1]->11:8, imm[11]->7
J: imm[20]->31, imm[10:1]->30:21, imm[11]->20, imm[19:12]->19:12
```

实现是纯位运算（`RVMinAsmBackend.cpp`）：

```cpp
Inst |= (Imm & 0x1000u) << 19;   // B: imm[12] -> bit31
Inst |= (Imm & 0x07e0u) << 20;   // B: imm[10:5] -> bits30:25
```

为什么要先读出现有指令字再改？因为 `Inst` 里已经有 opcode/rd/rs1/rs2/funct3，
fixup 只应该覆盖立即数位段。清位掩码 `0xfe000f80` / `0xfffff000` 就是这些
位段的并集。

### 3. 通用 fixup 也要处理

`MCAsmBackend::applyFixup` 在本树是**纯虚函数**，基类不会帮你写
`.word/.half` 的数据。通用 fixup 的路径是：

```cpp
MCFixupKindInfo Info = getFixupKindInfo(Fixup.getKind());
Value <<= Info.TargetOffset;
for (i < NumBytes) Data[i] |= (Value >> (8*i)) & 0xff;
```

还有一个容易漏的点：`maybeAddReloc` 必须在**所有** fixup 分支之前调用，
否则 `.word 未定义符号` 会静默写成 0 且不产生重定位：

```text
RVMin: relocations are not implemented yet (chapter 7)
```

这是第 7 章之前的预期行为：明确报错，而不是生成错误的 obj。

### 4. ELF 身份

```cpp
MCELFObjectTargetWriter(/*Is64Bit=*/false, OSABI, ELF::EM_RISCV,
                        /*HasRelocationAddend=*/true)
```

`EM_RISCV = 243`。如果这里写错，`llvm-readelf`/`llvm-objdump` 会把文件
当成未知架构——字节全对也没用。

## 门禁与验证

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop

ninja -C "$B" llc llvm-mc llvm-objdump llvm-readelf llvm-objcopy

# 67 条指令的编码差分 + .text 逐字节对比 + ELF machine 检查
"$W/llvm/lib/Target/RVMin/scripts/check-rvmin-encodings.sh" "$B/bin"
# PASS: 67 instructions, .text identical, ELF machine RISC-V
```

单条对照：

```bash
"$B/bin/llvm-mc" -triple=rvmin -show-encoding <<< 'addi a0, zero, 1'
# addi a0, zero, 1    # encoding: [0x13,0x05,0x10,0x00]
"$B/bin/llvm-mc" -triple=riscv32 -show-encoding <<< 'addi a0, zero, 1'
```

obj + 参考反汇编器回读：

```bash
cat > /tmp/t.s <<'ASM'
	.text
	.globl f
f:
	addi a0, zero, 1
	beq  a0, zero, done
	jal  ra, done
done:
	jalr zero, 0(ra)
ASM
"$B/bin/llvm-mc" -triple=rvmin -filetype=obj /tmp/t.s -o /tmp/t.o
"$B/bin/llvm-readelf" -h /tmp/t.o | grep Machine     # Machine: RISC-V
"$B/bin/llvm-objdump" -d /tmp/t.o
# 0: 00100513  li a0, 0x1
# 4: 00050863  beqz a0, 0xc
# 8: 008000ef  jal 0xc
# c: 00008067  ret
```

最后一行说明：输入的是 `jalr zero, 0(ra)`，参考反汇编器按别名显示为 `ret`，
编码一致。`llvm-objdump -d --triple=rvmin` 目前会报 “no disassembler”；
本章不实现反汇编器，回读统一用参考 RISCV 反汇编器。

## 踩过的坑

1. **AsmMatcher 要求字面量 token**
   `8(sp)` 必须解析成 `[imm, "(", reg, ")"]` 四个操作数，而不是两个。
   matcher 会拿 `(`、`)` 去匹配 `AsmString` 里的字面字符。
2. **伪指令不能进汇编器**
   `ADJCALLSTACKDOWN` 没有 mnemonic，`-gen-asm-matcher` 会报
   “has no tokens”。`RVMinPseudo` 需要 `isCodeGenOnly = 1`。
3. **ABI 寄存器名需要 `RegAltNameIndices`**
   只定义 `def ABIRegAltName` 不够；寄存器必须在
   `let RegAltNameIndices = [ABIRegAltName] in { ... }` 作用域里，
   否则生成的 alt-name 字符串表全是空的，打印出来是 `x10` 而不是 `a0`。
4. **`applyFixup` 是纯虚的**
   通用 `.word` 数据要自己写；不能调用基类实现（不存在）。
5. **`maybeAddReloc` 位置**
   必须放在所有分支之前，否则未定义符号不产生重定位也不报错。

## 练习

1. 给 `RVMinAsmBackend::writeNopData` 换一种合法 NOP
   （提示：RV32I 上任何 `addi x0, x0, 0` 之外的 4 字节指令都不保证是 NOP，
   想想为什么这里只接受 4 的倍数）。
2. 把 `simm12` 的 `ParserMatchClass` 临时改成 `ImmAsmOperand`，观察
   `addi a0, a0, 4096` 的报错位置和类型如何变化。
3. 手算 `beq a0, a1, -2` 的机器码，再用脚本验证。

## 本章改动

- 新增 MC 层 6 个文件与 AsmParser 子目录；
- `RVMinAsmPrinter` 接入 `emitInstruction`；
- `RVMinInstrInfo.td` 增加操作数匹配类；
- `RVMin.td` 注册 `AsmWriter`/`AsmParser`；
- 新增 `scripts/check-rvmin-encodings.sh` 与 67 条指令的编码语料。
