# 第 7 章：全局数据、字符串与重定位

## 目标

第 6 章之前，所有内存都是栈。本章接上静态存储：

```c
int counter;
int bump(void) { return ++counter; }

int table[4] = {1, 2, 3, 4};
int pick(int i) { return table[i]; }

const char msg[] = "hello";
```

完成本章后：

1. `@global`、`.data`/`.rodata` 初始化数据、字符串常量都能落到对象文件；
2. 全局地址用 RV32I 的 `LUI + ADDI` 物化成 `%hi/%lo` 对；
3. 汇编器能解析 `%hi(sym)`、`%lo(sym)`、`%lo(sym)(base)`；
4. ELF 里出现正确的 `R_RISCV_HI20`、`R_RISCV_LO12_I`、`R_RISCV_LO12_S`、
   `R_RISCV_32`、`R_RISCV_32_PCREL`；
5. `.eh_frame`（非 `nounwind` 函数）也能生成，且与重新汇编的结果逐字节一致。

## 代码地图

| 文件 | 本章改动 |
| --- | --- |
| `RVMinBaseInfo.h`（新） | `%hi/%lo` specifier、`MO_HI20/MO_LO12_I/MO_LO12_S` 目标标志 |
| `RVMinISelDAGToDAG.cpp` | `ISD::GlobalAddress` → `LUI %hi` + `ADDI %lo` |
| `RVMinMCInstLower.{h,cpp}` | `MO_GlobalAddress` → 带 specifier 的符号表达式 |
| `AsmParser/RVMinAsmParser.cpp` | 解析 `%hi(...)` / `%lo(...)` |
| `MCTargetDesc/RVMinMCAsmInfo.cpp` | 打印 `%hi(...)` / `%lo(...)` |
| `MCTargetDesc/RVMinMCCodeEmitter.cpp` | specifier → fixup kind（`%lo` 按 opcode 分 I/S） |
| `MCTargetDesc/RVMinFixupKinds.h` | `hi20` / `lo12_i` / `lo12_s` |
| `MCTargetDesc/RVMinAsmBackend.cpp` | 三个新 fixup 的位拼接 |
| `MCTargetDesc/RVMinELFObjectWriter.cpp` | fixup → ELF relocation 类型 |
| `scripts/rvmin-global-corpus.s` | `%hi/%lo` 与参考汇编器的字节门禁 |
| `llvm/test/CodeGen/RVMin/globals.ll` | 代码/数据/重定位的 FileCheck |

## 关键实现

### 1. RV32I 没有绝对寻址

RV32I 只有 `LUI`（写 `imm20 << 12`）和 `ADDI`（12 位）。任意 32 位地址拆成：

```text
addr = (hi20 << 12) + lo12      hi20 = (addr + 0x800) >> 12
                                lo12 = addr - (hi20 << 12)   ∈ [-2048, 2047]
```

`+0x800` 的舍入是关键：`ADDI` 的立即数是**符号扩展**的，所以 `lo12` 可能是
负数。经典组合：

| addr | hi20 | lo12 | 指令 |
| --- | --- | --- | --- |
| `0x00001000` | `1` | `0` | `lui 1` / `addi 0` |
| `0x00000fff` | `1` | `-1` | `lui 1` / `addi -1` |
| `0x12345678` | `0x12345` | `0x678` | `lui 0x12345` / `addi 0x678` |
| `0x12345fff` | `0x12346` | `-1` | `lui 0x12346` / `addi -1` |

第 4 章已经用同样的拆分物化常量；区别是常量在编译期已知，可以算出 `hi/lo`
放进指令，而全局地址要到链接期才确定，必须留下**重定位**：

```text
lui  a0, %hi(x)
addi a0, a0, %lo(x)
```

### 2. 选择：两个带目标标志的 GlobalAddress 节点

`ISD::GlobalAddress` 在 `Select()` 里展开。两个半边用不同的 target flags
区分，而不是自定义 SDNode：

```cpp
if (N->getOpcode() == ISD::GlobalAddress) {
  auto *GA = cast<GlobalAddressSDNode>(N);
  SDValue Hi = CurDAG->getTargetGlobalAddress(
      GV, DL, MVT::i32, Off, RVMinII::MO_HI20);
  SDValue Lo = CurDAG->getTargetGlobalAddress(
      GV, DL, MVT::i32, Off, RVMinII::MO_LO12_I);
  SDValue Lui = SDValue(CurDAG->getMachineNode(RVMin::LUI, DL, MVT::i32, Hi), 0);
  ReplaceNode(N, CurDAG->getMachineNode(RVMin::ADDI, DL, MVT::i32, Lui, Lo));
}
```

`MO_HI20` 让 `lui` 的操作数变成 `%hi20`；`MO_LO12_I` 让 `addi` 的操作数变成
`%lo12`。同一个 `GlobalAddress` 也能被用作 `SW` 的基址，所以 S 型需要
`MO_LO12_S`；本章的 store 直接用寄存器基址，暂未用到，但保留了这个标志。

注意 `getTargetGlobalAddress` 的 `Offset` 也要传给两半：`@arr+4` 的
`LO12` 是 `(arr+4)` 的低 12 位，而不是 `arr` 的低 12 位。

对于 `table[i]` 这种动态下标，DAG 会自动把地址算进寄存器：

```text
slli a0, a0, 2
lui  a1, %hi(table)
addi a1, a1, %lo(table)
add  a0, a1, a0
lw   a0, 0(a0)
```

### 3. MC 层：`MCSpecifierExpr`

本树的 MC 层用 `MCSpecifierExpr` 表示“带重定位修饰符的表达式”。我们定义
四个 specifier：

```cpp
enum Specifier : uint16_t {
  S_None = 0,
  S_HI20,
  S_LO12,     // 汇编文本里的 %lo，I/S 由 opcode 决定
  S_LO12_I,   // asm printer 已经知道是 I 型
  S_LO12_S,   // ... 或 S 型
};
```

打印由 `MCAsmInfo` 的虚函数完成：

```cpp
void RVMinMCAsmInfo::printSpecifierExpr(raw_ostream &OS,
                                        const MCSpecifierExpr &Expr) const {
  OS << '%' << rvmin::getSpecifierName(...) << '(';
  printExpr(OS, *Expr.getSubExpr());
  OS << ')';
}
```

解析则在 asm parser 里：

```cpp
if (getLexer().is(AsmToken::Percent)) {
  if (parseSpecifierOperand(Operands).isFailure())
    return true;
  return false;
}
```

`parseSpecifierOperand` 读 `%`、名字、`(`、表达式、`)`，必要时继续读
`(base)`，最后 `MCSpecifierExpr::create(...)`。`%hi/%lo` 在语法上就是普通
表达式，所以操作数谓词也必须接受它（见坑 2）。

### 4. fixup 与 ELF 重定位

`MCCodeEmitter` 看到表达式操作数时按 specifier 选 fixup：

| specifier | fixup | ELF 类型 |
| --- | --- | --- |
| `S_HI20` | `fixup_rvmin_hi20` | `R_RISCV_HI20` |
| `S_LO12_I` / `S_LO12` + load/ADDI | `fixup_rvmin_lo12_i` | `R_RISCV_LO12_I` |
| `S_LO12_S` / `S_LO12` + store | `fixup_rvmin_lo12_s` | `R_RISCV_LO12_S` |
| `fixup_rvmin_32` / `FK_Data_4` | — | `R_RISCV_32` |
| PC-relative `FK_Data_4` | — | `R_RISCV_32_PCREL` |
| `fixup_rvmin_branch` | — | `R_RISCV_BRANCH` |
| `fixup_rvmin_jal` | — | `R_RISCV_JAL` |

`applyFixup` 负责把值塞回指令字段：

```cpp
case fixup_rvmin_hi20:      // lui: bits 31:12
  Hi = (Value + 0x800) >> 12;
  Inst |= (Hi & 0xfffff) << 12;

case fixup_rvmin_lo12_i:    // I 型: bits 31:20
  Inst |= (Value & 0xfff) << 20;

case fixup_rvmin_lo12_s:    // S 型: 拆成两段
  Inst |= (Value & 0xfe0) << 20;   // imm[11:5] -> 31:25
  Inst |= (Value & 0x01f) << 7;    // imm[4:0]  -> 11:7
```

`hi20` 的 `+0x800` 舍入必须和 `lo12` 的符号扩展配套，否则地址会整体差 4096。

### 5. 静态数据与 `.eh_frame`

有了 MC 层，数据发射完全走 LLVM 的通用路径：`AsmPrinter::emitGlobalVariable`
负责 `.data`/`.rodata`、`.globl`、`.type`、`.size`、`.word`、`.asciz`；
我们只需要保证：

- `MCAsmInfo` 里有 `Data16bitsDirective` / `Data32bitsDirective`；
- ELF writer 能把数据 fixup 映射成 `R_RISCV_32`。

指针初始化 `@p = global ptr @x` 会变成：

```text
p:
	.word	x
```

结果：

```text
Relocation section '.rela.data':
 Offset     Info    Type            Sym. Value  Symbol's Name + Addend
00000004  00000301 R_RISCV_32     00000000   x + 0
```

非 `nounwind` 的函数会让 AsmPrinter 生成 `.eh_frame`。`.eh_frame` 里的
指针是 PC 相对的，因此产生 `R_RISCV_32_PCREL`：

```text
Relocation section '.rela.eh_frame':
 Offset     Info    Type                Sym. Value  Symbol's Name + Addend
0000001c  00000239 R_RISCV_32_PCREL   00000000   .text + 18
```

`R_RISCV_32_PCREL` 不需要新的 fixup：`FK_Data_4` + `IsPCRel` 已经在
ELF writer 里映射好了。本章之后，`llc -filetype=obj` 能处理普通的
C 函数（不带 `nounwind`）。

## 三个真实的坑

### 坑 1：`%lo` 分不清 I 型和 S 型

GNU 汇编里 `%lo(sym)` 两处都用同一个写法：

```asm
addi a0, a0, %lo(g)     # I 型
sw   a0, %lo(g)(sp)     # S 型，位布局完全不同
```

解析时无法从文本区分，所以 parser 只产生 `S_LO12`，由 `MCCodeEmitter`
看 opcode：

```cpp
Kind = (Opcode == SB || Opcode == SH || Opcode == SW)
           ? fixup_rvmin_lo12_s : fixup_rvmin_lo12_i;
```

asm printer 自己知道指令形状，所以直接发 `S_LO12_I`/`S_LO12_S`，不经过
这个判断。

### 坑 2：操作数谓词只认常量，`%hi` 被拒

parser 成功构造了表达式，`llvm-mc` 仍然报：

```text
error: invalid operand for instruction
        lui a0, %hi(x)
```

原因是 `.td` 里的谓词：

```cpp
bool isUImm20() const {
  int64_t Value;
  return evaluateConstantImm(Value) && isUInt<20>(Value);
}
```

`MCSpecifierExpr::evaluateAsAbsolute` 永远返回 false（值要链接期才知道），
谓词直接拒绝。需要显式放行符号形态：

```cpp
if (evaluateConstantImm(Value))
  return isUInt<20>(Value);
return isa<MCSpecifierExpr>(Imm);
```

`isSImm12` 同理。分支的 `isSImm13Lsb0`/`isSImm21Lsb0` 早就放行了
`MCSymbolRefExpr`，这里补上 `MCSpecifierExpr` 只是保持一致。

### 坑 3：`%lo(g)(a0)` 少了后缀解析

第一版 `parseSpecifierOperand` 只解析到 `)` 就返回，于是：

```text
error: unexpected token
        lw a1, %lo(g)(a0)
```

普通表达式路径在解析完表达式后会检查 `(` 并读取基址寄存器；specifier 路径
也要做同样的事。补上 `parseMemOpBaseReg` 之后，`%lo(sym)(base)` 就是两个
操作数：符号位移 + 基址寄存器。

教训：parser 里任何“新的操作数前缀”都要复制普通路径的尾部处理。

## 门禁与验证

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop
ninja -C "$B" llc clang llvm-mc llvm-objdump llvm-readelf llvm-objcopy FileCheck

"$B/bin/llvm-lit" -sv "$W/llvm/test/CodeGen/RVMin"
# Passed: 5 (basic, alu, control, stack, globals)

"$W/llvm/lib/Target/RVMin/scripts/check-rvmin-encodings.sh" "$B/bin"
# PASS: 83 instructions, .text identical (incl. label fixups and %hi/%lo),
#       ELF machine RISC-V
```

`globals.ll` 的关键输出：

```text
get_x:
	lui  a0, %hi(x)
	addi a0, a0, %lo(x)
	lw   a0, 0(a0)
	jalr zero, 0(ra)

set_x:
	lui  a1, %hi(x)
	addi a1, a1, %lo(x)
	sw   a0, 0(a1)
	jalr zero, 0(ra)
```

数据段：

```text
	.type	x,@object
	.data
	.globl	x
x:
	.word	42
	.size	x, 4

.Lstr:
	.asciz	"hello"
```

对象文件与重新汇编逐字节一致（`.text`、`.data`、`.rodata.str1.1`、
`.eh_frame`）：

```bash
"$B/bin/llc" -march=rvmin -mtriple=riscv32-unknown-elf -O1 ch7.ll \
  -filetype=obj -o a.o
"$B/bin/llc" -march=rvmin -mtriple=riscv32-unknown-elf -O1 ch7.ll -o a.s
"$B/bin/llvm-mc" -triple=rvmin -filetype=obj a.s -o b.o
for s in .text .data .rodata.str1.1 .eh_frame; do
  llvm-objcopy -O binary --only-section=$s a.o a.bin
  llvm-objcopy -O binary --only-section=$s b.o b.bin
  cmp a.bin b.bin
done
```

clang 端到端（`-O1` 和 `-O0` 都能出对象文件）：

```bash
"$B/bin/clang" --target=riscv32-unknown-elf -march=rv32i -mabi=ilp32 \
  -O1 -S -emit-llvm globals.c -o globals.ll
"$B/bin/llc" -march=rvmin -mtriple=riscv32-unknown-elf -O1 globals.ll \
  -filetype=obj -o globals.o
"$B/bin/llvm-readelf" -r globals.o
```

## 练习

1. 给 `RVMinAsmPrinter` 的全局数据加 `.p2align 2`，用 `llvm-readelf -S`
   确认段对齐发生变化。
2. 构造两个全局变量 `a`、`b`，让同一个函数取两次 `&a`，观察是否重复发
   `lui+addi`；思考把它 CSE 成一条需要放在哪个 pass。
3. 用 `llvm-readelf -r` 检查一个 `sw` 到全局数组的地址，确认它用的是
   `R_RISCV_LO12_S` 而不是 `LO12_I`。

## 当前限制（第 7 章）

- 字符串只能作为数据（`.asciz`）；`char` 读取需要 i8 的 `extload`，是第 9 章。
- 没有 PIC/PIE、GOT、TLS：只支持 `Reloc::Static`，`R_RISCV_CALL_PLT` 等
  调用相关重定位要等第 8 章。
- 没有链接器验证：`ld.lld` 未构建，重定位只做了对象级检查
  （`llvm-readelf -r`）。第 12 章可选地引入链接/QEMU。
- `.eh_frame` 只对“无栈帧”函数正确：本章没有发 `.cfi_def_cfa_offset` 之类
  的 CFI 指令，带栈帧的函数无法被展开器正确 unwind。CFI 属于第 11 章。
- 全局地址总是 `LUI+ADDI` 两条；同一函数里重复取同一地址还没有 CSE/重物化
  策略（第 13 章优化）。
