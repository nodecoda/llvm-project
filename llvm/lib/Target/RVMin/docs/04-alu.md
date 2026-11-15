# 第 4 章：整数算术、逻辑、移位、比较与 select

## 目标

第 3 章只能编译 `add` 和 12 位以内的常量。本章把 RV32I 的整数 ALU 全部接上：

```c
int sub(int a, int b)  { return a - b; }
int and_(int a, int b) { return a & b; }
int big(void)          { return 0x12345678; }
int cmp(int a, int b)  { return a < b; }
int sel(int a, int b)  { return a < b ? a : b; }
```

完成本章后：

1. `add/sub/and/or/xor/sll/srl/sra/slt/sltu` 及其中带 12 位立即数的形式都能选择；
2. 任意 32 位常量都能用 `LUI+ADDI` 物化（包括 `INT32_MIN`）；
3. 10 种整数比较（`eq/ne/lt/le/gt/ge` × 有符号/无符号）都能选择；
4. RV32I 没有条件传送，`select` 用无分支掩码序列实现；
5. `-O0` 也能编译（需要实现 `copyPhysReg`）。

## 代码地图

| 文件 | 本章改动 |
| --- | --- |
| `RVMinISelLowering.{h,cpp}` | `getSetCCResultType`、`SELECT`/`SELECT_CC` action、`lowerSELECT_CC` |
| `RVMinISelDAGToDAG.cpp` | `ISD::Constant` 的 `ADDI` / `LUI+ADDI` 物化 |
| `RVMinInstrInfo.td` | ALU/移位/比较的 `Pat`，`uimm5` 的 `ImmLeaf` |
| `RVMinInstrInfo.{h,cpp}` | `copyPhysReg`（`addi rd, rs, 0`） |
| `RVMinRegisterInfo.{h,cpp}` | 分配顺序；第 4 章保留 `ra` |
| `llvm/test/CodeGen/RVMin/alu.ll` | 本章 FileCheck 门禁 |

## 关键实现

### 1. 比较结果类型

本树 `TargetLowering` 默认的 `getSetCCResultType` 会给出 `i1` 一类的小类型，
而 RVMin 只有 i32 寄存器。直接让比较产生 i32：

```cpp
EVT RVMinTargetLowering::getSetCCResultType(const DataLayout &DL,
                                            LLVMContext &Context,
                                            EVT VT) const {
  return getPointerTy(DL);   // rv32 上是 i32
}
```

配合 `setBooleanContents(ZeroOrOneBooleanContent)`，`setcc` 的结果就是 0/1，
`zext i1` 可以免费消失（DAG 里已经是 i32）。

### 2. 比较指令序列

RV32I 只有 `SLT`/`SLTU`/`SLTI`/`SLTIU`，其余关系用一条 `XORI` 取反：

| IR | RV32I |
| --- | --- |
| `a < b`（有符号） | `slt a, b` |
| `a > b` | `slt b, a` |
| `a <= b` | `xori (slt b, a), 1` |
| `a >= b` | `xori (slt a, b), 1` |
| `a < b`（无符号） | `sltu a, b` |
| `a >= b`（无符号） | `xori (sltu a, b), 1` |
| `a == b` | `sltiu (xor a, b), 1` |
| `a != b` | `sltu zero, (xor a, b)` |

TableGen 里就是一张模式表：

```tablegen
def : Pat<(i32 (setlt (i32 GPR:$rs1), (i32 GPR:$rs2))),
          (SLT GPR:$rs1, GPR:$rs2)>;
def : Pat<(i32 (setle (i32 GPR:$rs1), (i32 GPR:$rs2))),
          (XORI (SLT GPR:$rs2, GPR:$rs1), 1)>;
def : Pat<(i32 (seteq (i32 GPR:$rs1), (i32 GPR:$rs2))),
          (SLTIU (XOR GPR:$rs1, GPR:$rs2), 1)>;
```

注意 `setcc` 的结果类型必须显式写成 `(i32 ...)`，否则 TableGen 类型推断会失败。

### 3. 任意 32 位常量：LUI + ADDI

RV32I 没有“装入立即数”指令。任何 32 位值都可以拆成：

```text
imm = (hi20 << 12) + lo12      lo12 ∈ [-2048, 2047]
```

`LUI rd, hi20` 把 `hi20` 写进 `rd[31:12]` 并**符号扩展** `rd[11:0]`，随后
`ADDI rd, rd, lo12` 修正低 12 位：

```cpp
int64_t Hi = (Imm + 0x800) >> 12;
int64_t Lo = Imm - (Hi << 12);
```

几个边界值：

| imm | hi20 | lo12 | 指令 |
| --- | --- | --- | --- |
| `0x12345678` | `0x12345` | `0x678` | `lui` + `addi 0x678` |
| `-0x123456` | `-0x123` | `-0x456` | `lui -0x123` + `addi -0x456` |
| `INT32_MIN` | `-0x80000` | `0` | `lui 0x80000` |
| `0x7ffff800` | `0x80000` | `-0x800` | `lui 0x80000` + `addi -0x800` |
| `0` | — | — | 直接使用 `x0` |

最后一行很重要：常量 0 必须映射到 `x0`，否则 `x = 0` 会变成一条多余的
`addi rd, zero, 0`，而且多占一个寄存器（本章第一版就踩了这个坑）。

选择逻辑放在手写 `Select()` 里，因为 TableGen 的 `Pat` 无法表达“根据常量范围
发不同指令序列”：

```cpp
if (N->getOpcode() == ISD::Constant) {
  int64_t Imm = cast<ConstantSDNode>(N)->getSExtValue();
  ...
}
```

### 4. select：无分支掩码

RV32I 没有 `czero`/`cmov`。本章用掩码实现：

```text
cond = setcc lhs, rhs      // 0 or 1
mask = 0 - cond            // 0x00000000 or 0xffffffff
res  = false ^ ((true ^ false) & mask)
```

对应 DAG：`SETCC` → `SUB` → `XOR`/`AND`/`XOR`。为什么要 `SELECT` 标
`Expand`、`SELECT_CC` 标 `Custom`？

- `SELECT` 展开后正好变成 `SELECT_CC`；
- `SELECT_CC` 由目标自己降级，不能反过来依赖 `SELECT`（否则
  LegalizeDAG 会报 “Cannot expand ISD::SELECT_CC when ISD::SELECT also needs
  to be expanded”）。

第 5 章有分支后，`select` 会改成分支形式，通常更短。

### 5. `-O0` 需要 copyPhysReg

`-O1` 以上的寄存器分配器可以消掉大部分 `COPY`，`-O0` 的 fast regalloc 不会。
没有 `copyPhysReg` 时：

```text
Target didn't implement TargetInstrInfo::copyPhysReg!
```

RV32I 的 `mv rd, rs` 就是 `addi rd, rs, 0`：

```cpp
BuildMI(MBB, MI, DL, get(RVMin::ADDI), DestReg)
    .addReg(SrcReg, getKillRegState(KillSrc))
    .addImm(0);
```

## 三个真实的坑

### 坑 1：`ra` 被当成临时寄存器

常量物化的虚拟寄存器被分配到 `x1`：

```asm
lui ra, 74565
addi ra, ra, 1656
add a0, a0, ra
jalr zero, 0(ra)     # ra 已经被破坏
```

第 4 章还没有栈可以保存/恢复 `ra`，所以：

1. 在 `getReservedRegs` 里保留 `X1`；
2. 把 `GPR` 的分配顺序改成 ABI 友好顺序（a0–a7 → t0–t2 → t3–t6 →
   s0–s1 → s2–s11 → zero/ra/sp/gp/tp）；
3. 第 6/8 章实现栈帧和调用后再放开 `ra`，由 PEI 负责保存。

### 坑 2：`ANDI/ORI/XORI` 的立即数是符号扩展的

`andi a0, a0, 4095` 无法编码（12 位字段符号扩展到 `0xfffffffffffff000` 这类值），
而 `andi a0, a0, -1` 编码的是 `& 0xffffffff`。所以 `simm12` 的 `ImmLeaf` 用
`isInt<12>(Imm)` 判断的是**符号扩展后的值**。位掩码 `0xff` 没问题；
`0xfff` 这种会退化成“先物化常量，再寄存器 AND”，正确但不最优。
真正的优化（`andi` 配合 `TrailingOnes` 分析）留到后续章节。

### 坑 3：移位量

RV32I 的移位指令只用低 5 位，LLVM 中移位量 >= 32 的结果是 poison，
所以 `(shl GPR, GPR)` 直接映射 `SLL` 是合法的。立即数形式需要 `uimm5` 的
`ImmLeaf`：

```tablegen
def uimm5 : Operand<i32>, ImmLeaf<i32, [{ return isUInt<5>(Imm); }]>;
def : Pat<(shl GPR:$rs1, uimm5:$shamt), (SLLI GPR:$rs1, uimm5:$shamt)>;
```

## 门禁与验证

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop
ninja -C "$B" llc llvm-mc llvm-objdump llvm-readelf FileCheck

"$B/bin/llvm-lit" -sv "$W/llvm/test/CodeGen/RVMin"
# Passed: 2 (basic.ll, alu.ll)

"$W/llvm/lib/Target/RVMin/scripts/check-rvmin-encodings.sh" "$B/bin"
# PASS: 67 instructions, .text identical, ELF machine RISC-V

for opt in -O0 -O1 -O2 -O3; do
  "$B/bin/llc" -march=rvmin -mtriple=riscv32-unknown-elf $opt ch4.ll -o /dev/null
done
```

用参考反汇编器回读本章输出：

```bash
"$B/bin/llc" -march=rvmin -mtriple=riscv32-unknown-elf -O1 ch4.ll \
  -filetype=obj -o ch4.o
"$B/bin/llvm-objdump" -d --triple=riscv32 ch4.o
```

节选（`li/seqz/snez/neg/zext.b` 是参考汇编器的别名，编码就是本章生成的
`addi/sltiu/sltu/sub/andi`）：

```text
  0: 123455b7  lui    a1, 0x12345
  4: 67858593  addi   a1, a1, 0x678
  8: 00b50533  add    a0, a0, a1
  ...
 88: 00b54533  xor    a0, a0, a1
 8c: 00153513  seqz   a0, a0          # sltiu a0, a0, 1
 98: 00a03533  snez   a0, a0          # sltu a0, zero, a0
```

## 练习

1. 给 `sltiu a0, a0, 1` / `sltu a0, zero, a0` 加 `seqz` / `snez` 的别名输出
   （提示：`RVMinInstPrinter` 里按 opcode 特判），并保证 `llvm-mc` 重汇编一致。
2. 用 `llc -mdebug-only=isel` 观察 `select` 的五条掩码指令是怎么生成的，
   然后手工构造 `select_cc`，看它是否走 `SELECT_CC` 的自定义 lowering。
3. 给 `xori` 加一个 `-2048/2047` 边界的 lit 测试，确认 `simm12` 谓词在
   两个端点上都正确。

## 当前限制（第 4 章）

- `select` 是无分支序列（4–5 条指令）；第 5 章分支就绪后改成分支形式。
- 大常量的寄存器分配还很朴素：每个常量一条 `LUI+ADDI`，没有常量池/重物化。
- `mul/div/rem` 未处理（第 10 章 libcall）；i8/i16/i64 合法化第 9 章。
- `ra` 仍被保留，函数还不能调用别人（第 8 章）。
