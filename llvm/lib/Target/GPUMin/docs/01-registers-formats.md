# 第 01 章：寄存器文件与 GM32 指令格式

## 目标

第 00 章只让 target "存在"：一条 `exit`，零个操作数。本章让 GM32 的
**操作数模型**存在——寄存器文件、编码格式和第一批指令：

1. 三类寄存器：`r0-r31`（每 lane）、`ur0-ur7`（每 warp uniform）、
   `p0-p7`（每 lane 谓词）；
2. 五种 32 位格式 R/I/M/B/J 的位域定义（M/B/J 只定义，本章不用）；
3. 15 条指令被 TableGen 接受，并能被 `llvm-mc` 逐字节编码：
   `mov`、`movi`、8 条整数 ALU、`setp.lt`、`setp.eq`、`ld`、`st`、`exit`；
4. 立即数字段有真实的符号性检查（`movi r1, 70000` 必须被拒绝）；
5. 不改 RVMin、不改 clang、不新建构建目录。

退出条件（可执行）：

```sh
llvm/lib/Target/GPUMin/scripts/check-ch01.sh <llvm-bin-dir>
```

五组检查：全部寄存器名可解析、R 格式位域落点、MOVI 符号扩展、
SETP/LD/ST/EXIT 编码、立即数越界被拒。

## 背景

### 为什么寄存器要分三类

一个 warp 里的 lane 不是 32 份独立的"线程上下文"那么简单：有些值每
lane 不同（`r`），有些值整个 warp 共享（`ur`），有些值是"每 lane 一位"
（`p`）。这三类在硬件上是三块不同的存储，在 LLVM 里也必须是三个不同的
`RegisterClass`：

| 类 | 成员 | LLVM 类型 | 分配单位 |
| --- | --- | --- | --- |
| `GPR` | `r0-r31` | `i32` | 每 lane 一份 |
| `UGPR` | `ur0-ur7` | `i32` | 每 warp 一份 |
| `PRED` | `p0-p7` | `i1` | 每 lane 一份 |

把它们塞进同一个类、用别名区分，会让 `computeRegisterProperties()` 推不出
正确的 spill 大小和 copy 代价，第 19 章的寄存器压力分析也就无从谈起。

### 为什么是固定 32 位

变长编码省指令空间，但代价是取指边界不确定：一个 warp 要"同时"发射
一条指令，取指单元必须先解码才知道下一条在哪。固定 32 位是 SIMT 教学
模型里最省事的选择，也是本章所有位域图的由来。

代价很直接：**立即数只有 16 位**。`movi` 装不下 32 位常量，第 04 章要
用 `movi` + `shl` 两条指令拼（或者放宽成两条 `movi`）。

### 为什么 `ld/st` 本章先用 I 格式

计划里 `ld/st` 属于 M 格式，字段是
`[10:8] space / [7:6] size / [5] sign`，位移只剩 11 位。本章先用 I 格式
（16 位位移、没有 space/size），原因是：**本章只证明"字段能落位、能编码"**，
M 格式的语义（地址空间、访存宽度）要到第 06 章才有东西可验证。到那时
两个定义和编码测试一起重写，读者能直接看到"字段被挤掉了"这个真实代价。

## 设计

### 编码布局

```text
R     31    26 25   21 20   16 15   11 10            0
      opcode     rd      rs1     rs2        funct

I     31    26 25   21 20   16 15                  0
      opcode     rd      rs1          imm16

MOVI  31    26 25   21 20   16 15                  0
      opcode     rd      0            imm16

LD    31    26 25   21 20   16 15                  0
      opcode     rd      rs1          imm16

ST    31    26 25   21 20   16 15                  0
      opcode    rs2      rs1          imm16

SETP  31    26 25   21 20   16 15   11 10            0
      opcode     pd      rs1     rs2        funct
```

`ST` 把"被存的值"放在 `rd` 槽（`[25:21]`），因为 store 没有目的寄存器，
这个槽本来空着；`SETP` 把谓词目的寄存器 `pd` 放在 `rd` 槽，同理。
这两处复用是解码器必须知道的第一条约定。

### opcode 表

| 值 | 名字 | 含义 | 本章定义 |
| --- | --- | --- | --- |
| `0x01` | `MOV` | 寄存器/特殊寄存器搬移 | `mov` |
| `0x02` | `ALU` | 整数运算，funct 选择操作 | 8 条 |
| `0x03` | `SETP` | 谓词比较 | `setp.lt`、`setp.eq` |
| `0x04` | `SELP` | 谓词选择 | 留给第 04 章 |
| `0x05` | `MOVI` | 立即数搬移 | `movi` |
| `0x08` | `LD` | 载入 | `ld` |
| `0x09` | `ST` | 存储 | `st` |
| `0x10` | `BRA` | 分支 | 第 05 章 |
| `0x11` | `CALL` | 调用 | 第 07 章 |
| `0x12` | `SYS` | 系统/退出/屏障 | `exit` |

`MOVI` 用独立 opcode，而不是复用 `MOV`：`MOV` 的 `[25:21]` 是 `rd`、
`[15:11]` 是 `rs2`，如果立即数形式也走 `0x01`，解码器就没有任何一位可以
区分"这是寄存器源还是立即数源"。给 `MOVI` 一个 opcode 是本章唯一一个
**偏离计划草案**的编码决定，代价是多用掉一个 opcode 值。

### funct 表

`ALU`（opcode `0x02`）的 `funct[10:0]`：

| funct | 3 | 2 | 1 | 0 | 指令 |
| --- | --- | --- | --- | --- | --- |
| 0 | 0 | 0 | 0 | 0 | `add` |
| 1 | 0 | 0 | 0 | 1 | `sub` |
| 2 | 0 | 0 | 1 | 0 | `and` |
| 3 | 0 | 0 | 1 | 1 | `or` |
| 4 | 0 | 1 | 0 | 0 | `xor` |
| 5 | 0 | 1 | 0 | 1 | `shl` |
| 6 | 0 | 1 | 1 | 0 | `shr` |
| 7 | 0 | 1 | 1 | 1 | `sar` |

`funct[10:4]` 保持 0；第 04 章的 `ALUI`（立即数形式）用
`funct[10] = 1` 标记。

`SETP`（opcode `0x03`）的 `funct` 是
`{ unsigned, relation[2:0] }`：

| funct | 关系 | 本章 |
| --- | --- | --- |
| 0 | `eq` | `setp.eq` |
| 1 | `ne` | — |
| 2 | `lt` | `setp.lt` |
| 3 | `le` | — |
| 4 | `gt` | — |
| 5 | `ge` | — |
| 8 | `eq`（unsigned） | — |

本章只定义有符号的 `lt/eq`；其余关系和 unsigned 位随第 04 章的
`SETCC` lowering 一起加。

### 指令清单（15 条）

| 汇编 | 格式 | 字段 |
| --- | --- | --- |
| `mov rd, rs1` | R1 | `funct=0`，`rs2=0` |
| `movi rd, imm16` | MOVI | 符号扩展 |
| `add/sub/and/or/xor/shl/shr/sar rd, rs1, rs2` | R2 | `funct=0..7` |
| `setp.lt pd, rs1, rs2` | SETP | `funct=2` |
| `setp.eq pd, rs1, rs2` | SETP | `funct=0` |
| `ld rd, imm(rs1)` | LD | 16 位有符号位移 |
| `st rs2, imm(rs1)` | ST | 16 位有符号位移 |
| `exit` | — | 整字 `0x48000000` |

### `GPUMinInstrFormats.td` 为什么要独立成文件

格式类是"位域模板"，它们继承 `GPUMinInst`。TableGen 按 **include 顺序**
自上而下求值，被继承的类必须先出现；第 00 章把基类写在
`GPUMinInstrInfo.td` 里，本章如果直接在另一个文件里写格式类并先 include，
就会报 `unknown class`。所以基类连同 opcode 表一起搬进
`GPUMinInstrFormats.td`，由 `GPUMinInstrInfo.td` 反向 include 它。

## 实现

### 1. `GPUMinRegisterInfo.td`

```tablegen
class GPUMinReg<bits<5> num, string n> : Register<n> {
  field bits<5> Num = num;
  let Namespace = "GPUMin";
  let HWEncoding{4-0} = num;
  let DwarfNumbers = [num];
}

foreach i = 0-31 in {
  def R#i : GPUMinReg<i, "r"#i>;
}
```

`HWEncoding` 是编码器取寄存器号的位置，`DwarfNumbers` 供调试信息使用。
`ur`/`p` 同理，但编码只有 3 位（8 个）。

```tablegen
def GPR : RegisterClass<"GPUMin", [i32], 32,
  (add R1, R2, R3,
       (sequence "R%u", 4, 11),
       ...
       R28, R29, R0, R30, R31)>;
```

分配顺序把 caller-saved 窗口放前面；`r0`（zero）、`r30`（sp）、`r31`（lr）
放最后。**注意 `R0/R30/R31` 仍然在 `GPR` 里**——它们"不可用"是靠
`getReservedRegs()` 表达的，不是靠"不放进分配类"。这两件事必须分开：

| 概念 | 表达 | 作用 |
| --- | --- | --- |
| 属于寄存器类 | `RegisterClass` 的成员 | 汇编器认识它、RA 能分配它 |
| 被保留 | `getReservedRegs()` | RA 不会分配它 |

本章的 bug 就出在把两者混为一谈（见"常见坑"第 1 条）。

### 2. `GPUMinInstrFormats.td`

```tablegen
class GPUMinInst<dag outs, dag ins, string asmstr, list<dag> pattern>
    : Instruction {
  field bits<32> Inst;
  ...
  let Size = 4;
}
```

`field bits<32> Inst` 是所有格式类共享的"整字"，每个格式类只负责把自己
那几个位段填进去：

```tablegen
class GPUMinR2<bits<6> op, bits<11> funct, dag outs, dag ins,
               string asmstr, list<dag> pattern = []>
    : GPUMinInst<outs, ins, asmstr, pattern> {
  bits<5> rd;
  bits<5> rs1;
  bits<5> rs2;
  let Inst{31-26} = op;
  let Inst{25-21} = rd;
  let Inst{20-16} = rs1;
  let Inst{15-11} = rs2;
  let Inst{10-0} = funct;
}
```

字段名（`rd`/`rs1`/`rs2`/`imm`）不是随便起的：TableGen 用它们把
`OutOperandList`/`InOperandList` 里的操作数和位段对上，**名字必须和
operand 名一致**。`GPUMinR1` 是"没有 `rs2` 的 R 格式"，直接把
`Inst{15-11}` 定成 0。

### 3. `GPUMinInstrInfo.td`

操作数类只需要一个：

```tablegen
class GPUMinImmAsmOperand<string name, string predicate> : AsmOperandClass {
  let Name = name;
  let PredicateMethod = predicate;
  let RenderMethod = "addImmOperands";
}

def SImm16AsmOperand : GPUMinImmAsmOperand<"SImm16", "isSImm16">;

def simm16 : Operand<i32> {
  let PrintMethod = "printOperand";
  let ParserMatchClass = SImm16AsmOperand;
}
```

`PredicateMethod` 指向手写 parser 里的 `isSImm16()`：

```cpp
bool isSImm16() const {
  if (Kind != Immediate)
    return false;
  if (auto *CE = dyn_cast<MCConstantExpr>(Imm))
    return isInt<16>(CE->getValue());
  return true;   // 符号操作数交给 fixup
}
```

符号操作数（`ld r1, sym(r2)`）在汇编阶段没有值，范围检查只能推迟到
链接/重定位；直接放行是正确做法，否则第 06 章的符号化全局地址没法写。

ALU 用 `multiclass` 生成，避免八份重复定义：

```tablegen
multiclass GPUMinALU<bits<11> funct, string mnemonic> {
  def "" : GPUMinR2<OPC_ALU.Value, funct, (outs GPR:$rd),
                    (ins GPR:$rs1, GPR:$rs2), mnemonic#" $rd, $rs1, $rs2", []>;
}

defm ADD : GPUMinALU<0x0, "add">;
...
defm SAR : GPUMinALU<0x7, "sar">;
```

本章新增的真实指令 `Pattern` 都是空列表：它们只被汇编器认识，还不参与
指令选择。第 03/04 章才把 SDNode 模式接上去——那时才会遇到
"TableGen 说 pattern 无效"的问题。

### 4. AsmParser

除了 `isSImm16()`，parser 不需要改动：寄存器操作数走生成的
`MatchRegisterName`，内存操作数走第 00 章已有的 `imm(reg)` 语法。
`AsmMatcher` 用 `ParserMatchClass` 决定每个 token 该试哪个
`AsmOperandClass`，试成功与否由 `PredicateMethod` 回答。

## 代码走读

### 一条 `add r4, r5, r6` 从文本到 4 个字节

```text
llvm-mc -triple=gpumin
 └─ GPUMinAsmParser::ParseInstruction()
     ├─ tryParseRegister("r4")        -> MatchRegisterName -> GPUMin::R4
     ├─ ...
     └─ MatchAndEmitInstruction()
         └─ MatchInstructionImpl()           // 生成的匹配表
             ├─ validateOperandClass(Op, MCK_GPR)
             │   └─ Table[RegNo] == MCK_GPR ?   // 关键：按寄存器号查表
             ├─ 命中 ADD: $rd=$r4, $rs1=$r5, $rs2=$r6
             └─ MCInst(ADD, {R4, R5, R6})
 └─ MCCodeEmitter::encodeInstruction()
     └─ getBinaryCodeForInstr()   // 用 Inst{...} 位段拼出 32 位
         └─ 0x08853000 按小端写 00 30 85 08
```

手工核对：

| 字段 | 值 | 移位 | 贡献 |
| --- | --- | --- | --- |
| opcode | `0x02` | `<<26` | `0x08000000` |
| rd=4 | 4 | `<<21` | `0x00800000` |
| rs1=5 | 5 | `<<16` | `0x00050000` |
| rs2=6 | 6 | `<<11` | `0x00003000` |
| funct | 0 | — | `0` |
| 合计 | | | `0x08853000` |

其余几条同理：

```text
mov  r1, r2       0x04220000   (op 0x01, rd 1, rs1 2, rs2 0, funct 0)
movi r3, 42       0x1460002a   (op 0x05, rd 3, imm 42)
setp.lt p1,r2,r3  0x0c221802   (op 0x03, pd 1, rs1 2, rs2 3, funct 2)
ld   r7, 8(r8)    0x20e80008   (op 0x08, rd 7, rs1 8, imm 8)
st   r9, 4(r10)   0x252a0004   (op 0x09, rs2 9, rs1 10, imm 4)
exit              0x48000000
```

### 为什么 `validateOperandClass` 是一个"按寄存器号索引的表"

`AsmMatcherEmitter` 会把**每个寄存器属于哪些 `MatchClassKind`** 提前算成
一张 `uint16_t Table[NUM_TARGET_REGS]`。运行期只做一次数组下标，就能回答
"这个寄存器是不是 GPR"。这张表的输入是**寄存器类的成员关系**，不是
指令定义。所以一个寄存器哪怕有名字、有编码，只要没进任何
`RegisterClass`，它在这张表里就是 `InvalidMatchClass`，汇编器会直接报
`invalid operand`——这正是本章开头那个 bug。

## 验证

```sh
$ ninja -C <build> llc llvm-mc
$ llvm/lib/Target/GPUMin/scripts/check-ch01.sh <build>/bin
PASS 1/5: r0-r31 and p0-p7 all parse
PASS 2/5: R-format opcode/rd/rs1/rs2/funct fields encode in place
PASS 3/5: MOVI sign-extends its 16-bit immediate
PASS 4/5: SETP compares, LD/ST displacements and EXIT encode as specified
PASS 5/5: out-of-range immediates are rejected
OK: chapter 01 gate passed
```

手工看产物：

```sh
$ printf 'st r9, 4(r10)\n' | llvm-mc -triple=gpumin -show-encoding
	st r9, 4(r10)                           # encoding: [0x04,0x00,0x2a,0x25]

$ printf 'movi r1, 70000\n' | llvm-mc -triple=gpumin
<stdin>:1:10: error: invalid operand for instruction
movi r1, 70000
         ^
```

`llvm-objdump -d` 仍然显示 `<unknown>`：反汇编器还是第 00 章的
"一律 Fail"。第 02 章补齐解码表之后，这两条路径才会对齐。

## 常见坑

1. **寄存器不在 `RegisterClass` 里，汇编器就不认它**。本章实现时
   `GPR` 从 `R4` 开始写（把 `r1-r3` 当成了"别处会加"），结果
   `mov r0, r5` 能过、`mov r1, r5` 报 `invalid operand`。生成文件里能
   直接看到证据：`Table[R1]` 是 `InvalidMatchClass`。教训是
   "`foreach` 生成了 32 个 `def`" 和 "32 个寄存器可分配" 是两件事。
2. **`AsmString` 里 `$name(` 会把括号读进变量名**。
   `"ld $rd, $imm($rs1)"` 报 `unable to find operand: 'imm('`，
   因为变量名字符集直到非字母数字才停。写 `${imm}(${rs1})` 或
   `"$imm ($rs1)"`。
3. **TableGen 对 include 顺序敏感**。格式类继承基类，基类必须先定义；
   把基类留在 `InstrInfo.td` 而格式类放前面，会报 `unknown class`。
4. **`isInt<16>` 需要 `MathExtras.h`**，且它接受的是 `int64_t`；
   `MCConstantExpr::getValue()` 在 32 位下是带符号的，别用
   `getValue() & 0xFFFF` 来"检查范围"。
5. **保留寄存器不是"不放进分配类"**。`r0/r30/r31` 必须在 `GPR` 里
   才有编码路径，否则 `llvm-mc` 认不出它们；不可分配性由
   `getReservedRegs()` 负责。
6. **`MOVI` 必须独立 opcode**。`MOV` 的字段已经全部占满（`rd`、`rs1`、
   `rs2`、`funct`），没有"立即数位"可用；想复用 `0x01` 就得牺牲一个
   操作数字段，解码器才有判据。
7. **`ST` 用 `rd` 槽存 `rs2`，`SETP` 用 `rd` 槽存 `pd`**。写解码器时
   如果按"`rd` 一定是目的"去解，这两个格式会反；第 02 章要按格式类
   分别处理。
8. **`PRED` 的第三个模板参数是 alignment，不是 spill 大小**。
   `RegisterClass<..., [i1], 1, ...>` 里的 `1` 只影响对齐；spill 大小由
   `[i1]` 推出来，第 19 章做寄存器压力时才需要复查这个值。

## 练习

1. 把 `movi` 从 `0x05` 挪回 `0x01`，并在 `funct` 里加一位标记立即数形式。
   写出新的位域图，说明为什么 `rs2` 字段必须让出来。
2. 增加 `setp.ne`（funct 1）和 `setp.ge`（funct 5）。只改 `.td` 够吗？
   `check-ch01.sh` 需要加哪几条？
3. 把 `GPUMinR1` 的 `Inst{15-11}` 从 0 改成 1，观察 `mov` 的编码变化。
   想一想：解码器看到 `0x01` + `rs2 != 0` 应该做什么？
4. 给 `ld/st` 加一个 `.b8` 后缀（`ld.b8 rd, imm(rs1)`），先只改
   `AsmString`，再想清楚它需要 M 格式的哪些位（第 06 章）。
5. 用 `llvm-tblgen -gen-asm-matcher` 单独生成
   `GPUMinGenAsmMatcher.inc`，找到 `Table[NUM_TARGET_REGS]`，对照
   `GPUMinGenRegisterInfo.inc` 的枚举顺序，验证"表是按寄存器号索引"。

## 下一章

第 02 章把 MC 层补齐：反汇编表、`gpumin-encode.py` 独立编码器、
本章这批指令的逐字节一致性和"汇编→反汇编→汇编"往返。本章的位域图会在
那一章第一次被**双向**验证，而不只是编码方向。
