# 第 02 章：MC 层双向闭环

## 目标

第 01 章让"汇编 → 字节"成立。本章让**反方向**也成立，并且用两种互相
独立的方式验证：

1. `llvm-mc -disassemble` 真正解码 GM32，而不是第 00 章的"一律 Fail"；
2. `tools/gpumin-encode.py` 是一份**不共享 LLVM 代码**的编码器，
   从 ISA 描述重新实现一遍；
3. 23 条指令的编码与 Python 实现逐字节一致；
4. `asm → bytes → asm → bytes` 是恒等变换；
5. 未知字只报一次 `invalid instruction encoding`，不会逐字节重同步。

退出条件（可执行）：

```sh
llvm/lib/Target/GPUMin/scripts/check-ch02.sh <llvm-bin-dir>
```

## 背景

### 为什么"编码器和解码器都对"还不够

`llvm-mc -show-encoding` 和 `-disassemble` 来自同一份 `.td`：
`-gen-emitter` 和 `-gen-disassembler` 读的是同一组 `Inst{}` 位段。如果
位段写错了——比如 `funct` 应该放 `[10:8]` 却写成了 `[9:7]`——两边会
**一起错**，而 "assemble → disassemble → assemble" 仍然是恒等。

这类错误只能靠**第三个实现**抓。本章的 `gpumin-encode.py` 就是那个
实现：它不读 `.td`、不链接 LLVM，只按第 01 章的位域图手写编码。
三者的关系：

| 检查 | 能抓住的错误 | 抓不住的错误 |
| --- | --- | --- |
| `llvm-mc` 编码 | 语法、立即数范围 | 位域布局 |
| 解码 round-trip | 解码器的符号/范围错误 | 与编码器共享的位域错误 |
| Python 对拍 | 位域布局、字段顺序 | 语义（本章还没有语义） |

### 为什么本章还不碰 ELF

我们用 `llvm-mc -disassemble` 而不是 `llvm-objdump -d` 做验证。原因是
对象文件里没有"GPUMin"这个 machine 号：第 00 章暂时借用 `EM_RISCV`，
于是 `llvm-objdump` 会把文件当 RISC-V 打开，走 RISC-V 的反汇编器，输出
一串 `<unknown>`。机器号和 `ELFObjectFile` 的 arch 映射是第 03 章的内容，
本章的验证路径刻意绕开它。

## 设计

### 解码树从哪来

`-gen-disassembler` 把 `.td` 里的每条指令的固定位（opcode、funct、
`rs2=0` 这类约束）编成一棵前缀树：

```text
switch Inst[31:26]
  case 0x01:                       // MOV
    check Inst[15:0] == 0x0        // rs2 和 funct 都固定为 0
    decode MOV
  case 0x02:                       // ALU
    switch Inst[10:0]              // funct
      case 0: decode ADD
      case 1: decode SUB
      ...
```

注意 MOV 那条 `check Inst[15:0] == 0`：`GPUMinR1` 把 `Inst{15-11}` 定成 0，
`funct` 也是 0，生成器就把"这些位必须为 0"变成匹配条件。**没有这个
约束，MOV 会和同 opcode 下的其它形式重叠**——第 03 章加 `mov rd, ur` 时
就会撞上。

### 生成器不知道的三件事

| 字段 | 生成器看到的 | 真实含义 | 由谁补 |
| --- | --- | --- | --- |
| `rd`/`rs1`/`rs2` | 5 位整数 | 每 lane 寄存器 `r0-r31` | `DecodeGPRRegisterClass` |
| `ur` 字段 | 3 位整数 | 每 warp 寄存器 `ur0-ur7` | `DecodeUGPRRegisterClass` |
| `pd` | 3 位整数 | 谓词寄存器 `p0-p7` | `DecodePREDRegisterClass` |
| `imm16` | 16 位整数 | **有符号**立即数 | `DecodeSImm16` |

前三个的挂载点在 `.td` 的寄存器类上：

```tablegen
def GPR : RegisterClass<"GPUMin", [i32], 32, (...)> {
  let DecoderMethod = "DecodeGPRRegisterClass";
}
```

第四个挂在操作数上：

```tablegen
def simm16 : Operand<i32> {
  let PrintMethod = "printOperand";
  let ParserMatchClass = SImm16AsmOperand;
  let DecoderMethod = "DecodeSImm16";
}
```

字符串必须和 C++ 里的函数名完全一致；写错不会在 TableGen 阶段报错，
而是在链接期变成 `undefined symbol`。

### `Size = 4` 的策略

解码失败时 `Size` 仍然给 4：

```cpp
DecodeStatus Result = decodeInstruction(DecoderTable32, MI, Insn, Address,
                                        this, STI);
Size = 4;
return Result;
```

如果失败时给 0，`llvm-objdump` 会在 +1、+2、+3 各重试一次，一个坏字
打印四行 `<unknown>`。GM32 是固定长度，一个 word 永远是一条指令，失败
也占满 4 字节——这条规则在第 00 章 §"常见坑"里已经出现过一次（当时是
`writeNopData`），这里是它的解码侧版本。

## 实现

### 1. `.td` 侧（4 行）

`GPUMinRegisterInfo.td` 给三个寄存器类各加一行 `DecoderMethod`；
`GPUMinInstrInfo.td` 给 `simm16` 加一行。没有别的改动——编码器完全不
需要动，它本来就知道字段。

### 2. 寄存器表

三个寄存器文件在 `.td` 里是连续的（`foreach` 生成），所以解码器可以
直接用编码值查表：

```cpp
static const unsigned GPRDecoderTable[] = {
    GPUMin::R0, GPUMin::R1, ..., GPUMin::R31};

static DecodeStatus DecodeGPRRegisterClass(MCInst &Inst, uint64_t RegNo,
                                           uint64_t Address,
                                           const MCDisassembler *Decoder) {
  if (RegNo >= std::size(GPRDecoderTable))
    return MCDisassembler::Fail;
  Inst.addOperand(MCOperand::createReg(GPRDecoderTable[RegNo]));
  return MCDisassembler::Success;
}
```

三个函数形状相同，只有表不同。这比 `switch` 短，也让"编码值 → 寄存器"
这一层关系可以被一个循环验证（见练习 5）。

### 3. 符号扩展

```cpp
static DecodeStatus DecodeSImm16(MCInst &Inst, uint64_t Imm, int64_t Address,
                                 const MCDisassembler *Decoder) {
  Inst.addOperand(MCOperand::createImm(SignExtend64<16>(Imm)));
  return MCDisassembler::Success;
}
```

不做的后果很具体：`movi r31, -1` 编码为 `0x17e0ffff`，反汇编会打印
`movi r31, 65535`，round-trip 立刻失败。

### 4. `getInstruction`

```cpp
DecodeStatus GPUMinDisassembler::getInstruction(...) const {
  Size = 0;
  if (Bytes.size() < 4)
    return MCDisassembler::Fail;

  uint32_t Insn = support::endian::read32le(Bytes.data());
  DecodeStatus Result =
      decodeInstruction(DecoderTable32, MI, Insn, Address, this, STI);
  Size = 4;
  return Result;
}
```

`DecoderTable32` 这个名字来自生成文件（32 位固定长度这一组的表名）。
它不是 `#define`，也不是可配置项：写错就是 `undefined symbol`，
所以新 target 接解码器时先去 `.inc` 里确认表名。

### 5. `tools/gpumin-encode.py`

结构是表驱动的，和 `.td` 一一对应，但**独立书写**：

```python
OPC  = {"mov": 0x01, "alu": 0x02, "setp": 0x03,
        "movi": 0x05, "ld": 0x08, "st": 0x09, "exit": 0x12}
ALU  = {"add": 0, ..., "sar": 7}
SETP = {"eq": 0, "ne": 1, "lt": 2, ...}

def word_r(op, rd, rs1, rs2, funct):
    return (op << 26) | (rd << 21) | (rs1 << 16) | (rs2 << 11) | funct

def word_i(op, rd, rs1, imm):
    if not -32768 <= imm <= 32767:
        raise AsmError(...)
    return (op << 26) | (rd << 21) | (rs1 << 16) | (imm & 0xFFFF)
```

它支持第 01 章的语法，包括 `ld r7, -4(r2)` 这种带符号位移；第 06 章把
访存改成 M 格式后，语料已经换成 `ld.global.b32 r7, 8(r8)` 一类写法。
`--selftest` 里嵌了 8 条手算的期望值：那是本章唯一的"人工基准"，
其余 15 条由与 `llvm-mc` 的对拍覆盖。

## 代码走读

### 一次 `llvm-mc -disassemble`

```text
printf '0x00 0x30 0x85 0x08' | llvm-mc -triple=gpumin -disassemble
 └─ MCAsmParser? 不，走 MCDisassembler::getInstruction
     ├─ read32le -> 0x08853000
     ├─ decodeInstruction(DecoderTable32, ...)
     │   ├─ switch Inst[31:26] = 0x02      -> ALU
     │   ├─ switch Inst[10:0]  = 0x000     -> ADD
     │   ├─ DecodeGPRRegisterClass(rd=4)   -> R4
     │   ├─ DecodeGPRRegisterClass(rs1=5)  -> R5
     │   └─ DecodeGPRRegisterClass(rs2=6)  -> R6
     └─ MCInst(ADD, {R4, R5, R6})
 └─ MCInstPrinter::printInstruction -> "\tadd r4, r5, r6"
```

打印走的是 `AsmString` 的反向渲染：`printOperand` 把 `MCOperand` 变回
`r4`/`-4` 这样的文本。它和编码器共享 `AsmString`，所以"打印格式"和
"解析格式"不会漂移。

### round-trip 到底证明了什么

```text
asm --[encoder]--> bytes --[decoder]--> asm' --[encoder]--> bytes'
```

`bytes' == bytes` 证明：解码器认识的每条指令，重新编码后位完全相同。
它**不**证明 `asm'` 和 `asm` 文本一样（`4(r10)` 可能打印成 `4(r10)`，
也可能打印成别的等价形式），所以 gate 比较的是字节，不是文本。

## 验证

```sh
$ ninja -C <build> llc llvm-mc
$ llvm/lib/Target/GPUMin/scripts/check-ch02.sh <build>/bin
PASS: Python encoder self-test (8 instructions)
PASS 1/4: 23 encodings match the independent encoder
PASS 2/4: every encoded word decodes to one instruction
PASS 3/4: asm -> bytes -> asm -> bytes is an identity
PASS 4/4: unknown words are reported once, without resynchronising
OK: chapter 02 gate passed
```

手工看一对：

```sh
$ printf 'ld r1, -4(r2)\n' | llvm-mc -triple=gpumin -show-encoding
	ld r1, -4(r2)                           # encoding: [0xfc,0xff,0x22,0x20]

$ printf '0xfc 0xff 0x22 0x20\n' | llvm-mc -triple=gpumin -disassemble
	ld r1, -4(r2)
```

## 常见坑

1. **`MCDecoderOps.h` 和 `using namespace llvm::MCD`**。生成表里的
   `OPC_Decode`/`OPC_CheckField` 都带 `MCD::` 前缀，两个都要有，否则
   报 `'OPC_Decode' was not declared`。
2. **`DEBUG_TYPE` 必须定义在 include 之前**。`MCDecoder.h` 里有
   `LLVM_DEBUG`，忘了定义会得到一串
   `'DEBUG_TYPE' was not declared`，而且报错点在头文件里，很容易看错。
3. **`DecoderMethod` 是字符串契约**。`.td` 里写 `DecodeSImm16`、C++ 里
   写 `decodeSImm16`，TableGen 全绿、链接失败。命名统一成
   `Decode<Thing>` 可以少踩一半。
4. **失败路径也要给 `Size`**。第 00 章的 `writeNopData` 坑在这里有个
   孪生兄弟：给 0 会让 objdump 重同步。
5. **`RegisterClass` 的 `DecoderMethod` 只影响解码**。编码仍然走
   `HWEncoding`；两者不一致时 round-trip 才会暴露，所以别只测编码。
6. **测试脚本解析 `[0x..]` 时别写 `[0-9a-f,]`**。它匹配不到 `0x` 里的
   `x`——本章的 gate 第一版就栽在这里，`grep -o '\[[^]]*\]'` 更稳。
7. **`llvm-mc -show-encoding` 的输出在 stdout**，但它前面有一个 tab；
   按列切割会拿到空串，用 "找方括号" 的方式提取。

## 练习

1. 给 `mov` 增加 `mov rd, ur_src`（uniform → lane）。你需要新格式还是
   复用 `GPUMinR1`？先写出位域图，再想解码器怎么区分两种 `mov`。
2. 把 `DecodeSImm16` 改成零扩展（`Imm` 直接 `createImm`），重跑
   `check-ch02.sh`，看是哪一步先失败，解释为什么不是第 1 步。
3. 在 `gpumin-encode.py` 里把 `ALU` 的 `funct` 整体加 1，重跑 gate。
   对拍和 round-trip 各报什么？这能说明两种测试的分工。
4. 用 `llvm-mc -triple=gpumin -filetype=obj` 生成 `.o`，再
   `llvm-objdump -d`。解释为什么输出是 `<unknown>`，以及第 03 章要改
   哪两处（提示：ELF machine 号 + `ELFObjectFile` 的 arch 映射）。
5. 写一个循环，用 `llvm-mc -disassemble` 解码 0..65535 里所有 opcode
   为 `0x02`、funct 为 0..7 的字，验证"每个 funct 恰好解出一条指令"。
   如果某个 funct 解出两条，说明 `.td` 里有重叠。

## 下一章

第 03 章把这条链路接到真实对象文件上：`mov`/`ret` 的指令选择、ELF
写出、machine 号，以及 `llvm-objdump -d` 第一次能真正反汇编 GPUMin
的 `.o`。本章的 round-trip 会从 `llvm-mc` 内存路径扩展到磁盘路径。
