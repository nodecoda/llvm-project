# 第 1 章：寄存器与 RV32I 指令描述

## 目标

把 `rvmin` 从“只有 target 骨架”推进到“TableGen 里有一套真实、可编码的
RV32I 指令描述”：

1. `RVMinInstrFormats.td` 定义 R / I / S / B / U / J 六种编码格式；
2. `RVMinInstrInfo.td` 定义核心指令、立即数操作数与指令语义标志；
3. TableGen 无错误，`llc` 仍能构建，空模块流水线保持可用。

本章**不**实现指令选择、汇编输出和编码器——那些分别在第 3、2 章。
但本章的位域布局必须一次写对，因为后面所有代码都建立在它上面。

## RV32I 的六种格式

RISC-V 的所有 32 位指令都是定长 32 bit，操作码在最低 7 位。
指令手册用“高位在左”的写法描述字段，我们沿用同样的顺序：

```text
R   31        25 24    20 19    15 14  12 11     7 6        0
    funct7       rs2       rs1      funct3    rd       opcode

I   31            20 19    15 14  12 11     7 6        0
    imm[11:0]        rs1      funct3    rd       opcode

S   31     25 24    20 19    15 14  12 11     7 6        0
    imm[11:5]  rs2       rs1      funct3    imm[4:0]  opcode

B   31 30    25 24    20 19    15 14  12 11  8 7  6        0
    imm[12|10:5] rs2      rs1      funct3 imm[4:1|11] opcode

U   31            12 11     7 6        0
    imm[31:12]       rd       opcode

J   31 30        21 20 19        12 11     7 6        0
    imm[20|10:1|11|19:12]   rd       opcode
```

对应到 TableGen：

| 类 | 字段 | 位域 |
| --- | --- | --- |
| `RVMinRInst` | `rd` `rs1` `rs2` `funct3` `funct7` `opcode` | 直接拼接 |
| `RVMinIInst` | `rd` `rs1` `imm12` `funct3` `opcode` | `Inst{31-20}=imm12` |
| `RVMinIShiftInst` | `rd` `rs1` `shamt` `funct7` `funct3` `opcode` | `Inst{24-20}=shamt` |
| `RVMinSInst` | `rs2` `rs1` `imm12` `funct3` `opcode` | `imm12{11-5}` / `{4-0}` 分两段 |
| `RVMinBInst` | `rs1` `rs2` `imm13` `funct3` `opcode` | 按 `imm[12\|10:5\|4:1\|11]` 拆位 |
| `RVMinUInst` | `rd` `imm20` `opcode` | `Inst{31-12}=imm20` |
| `RVMinJInst` | `rd` `imm21` `opcode` | 按 `imm[20\|10:1\|11\|19:12]` 拆位 |

### 与 llvm/lib/Target/RISCV 的一个刻意差异

官方 RISCV 后端把 B/J 型立即数先右移一位，存成 12/20 bit
（`imm12`/`imm20`），编码器再左移回去。RVMin 直接存**完整字节偏移**
（`imm13`/`imm21`，bit 0 恒为 0），并把“必须是 2 字节对齐”写进
`MCOperandPredicate`。

原因：教程后面的 `LowerBR_CC` / `LowerGlobalAddress` 直接给出字节偏移，
少一次移位就少一个出错点。代价是 B/J 字段各多 1 bit 的 TableGen 记录，
编码结果完全一样。

`Inst` 里的位编号从 0（LSB）开始，所以 `Inst{6-0}` 就是 opcode、
`Inst{11-7}` 就是 `rd`。TableGen 的 `-print-records` 可以直接检查：

```bash
B=/home/ubuntu/llvm/builds/pr-ctpop
"$B/bin/llvm-tblgen" -I llvm/include -I llvm/lib/Target/RVMin \
  -print-records llvm/lib/Target/RVMin/RVMin.td | grep -A1 '^def ADDI'
```

## 指令清单

| 类别 | 指令 |
| --- | --- |
| R 型 ALU | `ADD SUB AND OR XOR SLL SRL SRA SLT SLTU` |
| I 型 ALU | `ADDI ANDI ORI XORI SLTI SLTIU` |
| I 型移位 | `SLLI SRLI SRAI` |
| U 型 | `LUI AUIPC` |
| 跳转 | `JAL JALR` |
| 分支 | `BEQ BNE BLT BGE` |
| 访存 | `LW SW` |

第 6 章会补 `LB LH LBU LHU SB SH`，第 5 章会补 `BLTU BGEU`。
立即数形式的逻辑/移位指令虽然计划表里写在第 4 章“选择”，
但编码格式与 `ADDI` 完全相同，所以定义放在本章一次写全。

## 操作数与约束

```tablegen
def simm12 : Operand<i32> { /* isInt<12> */ }
def uimm5  : Operand<i32> { /* isUInt<5>  */ }
def uimm20 : Operand<i32> { /* isUInt<20> */ }
def simm13_lsb0 : Operand<i32> { /* isShiftedInt<12, 1> */ }
def simm21_lsb0 : Operand<i32> { /* isShiftedInt<20, 1> */ }
```

- `MCOperandPredicate` 在编码器/汇编器里做最后一道校验：越界立即数不会
  被静默截断。
- 操作数名必须与 `Inst` 的字段名一致（`$rs1` → `bits<5> rs1`），
  否则 TableGen 生成编码器时会报 “operand does not appear in encoding”。
- `uimm20` 是 `LUI/AUIPC` 的 `imm[31:12]` 原值，不是已经左移的地址。

`JALR` 的汇编写法是 `jalr rd, imm(rs1)`，TableGen 的 `AsmString`
用 `${imm12}(${rs1})` 的花括号消歧义。

## 语义标志

格式类只描述编码；指令必须自己声明语义，否则后面的优化/调度会误判：

| 指令 | 标志 |
| --- | --- |
| ALU/移位/U 型 | `hasSideEffects=0, mayLoad=0, mayStore=0` |
| `LW` | `mayLoad=1` |
| `SW` | `mayStore=1, hasSideEffects=1` |
| `BEQ/BNE/BLT/BGE` | `isBranch=1, isTerminator=1, isBarrier=1` |
| `JAL/JALR` | `isTerminator=1, isBarrier=1` |

`JALR` 还是间接跳转，等第 5 章真正发射控制流时再加 `isIndirectBranch`。

## 门禁与验证

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop

ninja -C "$B" llc
"$B/bin/llc" --version | grep -w rvmin

# 空模块仍应成功
printf 'target triple = "riscv32-unknown-unknown-elf"\n' > /tmp/empty.ll
"$B/bin/llc" -march=rvmin -mtriple=riscv32-unknown-elf -filetype=null /tmp/empty.ll

# 非空函数此时应停在占位 pass（第 3 章移除）
printf 'define i32 @f() { ret i32 0 }\n' > /tmp/f.ll
"$B/bin/llc" -march=rvmin -mtriple=riscv32-unknown-elf -filetype=null /tmp/f.ll
# LLVM ERROR: RVMin: instruction selection is not implemented yet
```

手工核对编码（与 `llvm-mc -triple=riscv32 -show-encoding` 一致）：

| 汇编 | 机器码（小端字节） |
| --- | --- |
| `addi a0, zero, 1` | `13 05 10 00` |
| `add a2, a0, a1` | `33 06 b5 00` |
| `lui a1, 0x12345` | `b7 55 34 12` |
| `sw a2, 8(sp)` | `23 24 c1 00` |
| `beq a0, a1, 8` | `63 04 b5 00` |
| `jal 16` | `ef 00 00 01` |

以 `addi` 为例手算：

```text
opcode=0010011  rd=01010  funct3=000  rs1=00000  imm=000000000001
Inst = 000000000001 00000 000 01010 0010011 = 0x00100513
小端：13 05 10 00
```

## 练习

1. 用 `-print-records` 找出 `SLLI` 的 `Inst` 位域，确认 `funct7=0000000`
   落在 bit 31–25、`shamt` 落在 bit 24–20。
2. 手算 `sw a2, 8(sp)` 的 32 位值，解释为什么 `imm[11:5]` 全 0、
   `imm[4:0]=01000`。
3. 如果把 `simm13_lsb0` 换成 `isInt<13>`，哪些非法分支偏移会被放过？
   为什么这会在第 5 章变成难以定位的 bug？

## 本章改动

- `RVMinInstrFormats.td`：opcode、六种格式、`TSFlags` 格式标记；
- `RVMinInstrInfo.td`：立即数操作数、30 条指令、语义标志、伪指令保留；
- `RVMinRegisterInfo.td`：无改动（`x0`–`x31` 与 ABI 别名在第 0 章已就绪）。
