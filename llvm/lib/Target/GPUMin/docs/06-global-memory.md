# 第 06 章：M 格式与 global memory

## 目标

第 01 章把 `ld/st` 临时塞进 I 格式：一个 16 位位移，没有空间、没有宽度。
本章把它换成真正的 M 格式，并第一次让 IR 里的访存落到 GM32 指令上：

1. 一个 16 位字段里同时放 **11 位字节位移 + space + size + sign**；
2. `ld.global.b32` / `ld.global.s8` / `st.shared.b16` 等 29 条指令，
   每个 space/size 组合都是独立 opcode；
3. 位移是**字节**偏移，无符号/符号扩展由 sign 位显式编码；
4. `addrspace(1)` 的 `load`/`store` 选择 global 形式，常量 GEP 折进位移；
5. 寄存器索引地址走 `movi + shl + add`，再用 `0(base)` 访存；
6. 编码器、反汇编器、独立 Python 编码器三方一致，round-trip 是恒等。

退出条件（可执行）：

```sh
llvm/lib/Target/GPUMin/scripts/check-ch06.sh <llvm-bin-dir>
```

## 背景

### 为什么 I 格式不够用

第 01 章的 `ld r7, 8(r8)` 只有两种信息：哪个寄存器、偏移多少。真实 GPU
访存至少还要回答三个问题：

- **哪个地址空间**：global / shared / local / constant；
- **访问多宽**：8 / 16 / 32 位；
- **窄加载怎么扩展**：零扩展还是符号扩展。

这些都不能从寄存器名或指令上下文推出来，必须进指令字。I 格式的 16 位
已经全给了位移，所以 M 格式必须重新分配这 16 位。

### 11 + 2 + 2 + 1 = 16

```text
M: [31:26] opcode | [25:21] rd/rs2 | [20:16] base |
   [15:5] imm11 | [4:3] space | [2:1] size | [0] sign
```

- `imm11`：有符号字节位移，范围 −1024..+1023；
- `space`：`0` global、`1` shared、`2` local、`3` constant；
- `size`：`0` b8、`1` b16、`2` b32；
- `sign`：load 的 `0` 零扩展 / `1` 符号扩展，store 恒为 0。

计划草案写的是 `funct[10:8] space / [7:6] size / [5] sign`。按那个布局，
funct 占 6 位，位移只剩 10 位——草案里"shrinks the displacement to 11 bits"
和它自己的字段宽度对不上。把三个限定符挪到最低 5 位之后，16 位正好分完，
位移还多赚 1 位。这一章按后者实现，并在
`GPUMinInstrFormats.td` 的注释里记录了这个取舍。

### 为什么位移按字节，而不是按元素

GPU ISA 的访存单位是字节：`st.global.b16` 存两个字节，`st.global.b32`
存四个。如果位移按元素计，同一份 `gep` 结果在 b8/b16/b32 下含义不同，
模拟器和编译器都要额外维护一张比例表；按字节计，地址算术是唯一真相源。

代价是编译器必须在 GEP 里乘 element size。这不是额外工作：LLVM 的
`getelementptr i32, ptr %p, i32 2` 本来就产出 `p + 8`，SelectionDAG 看到
的已经是字节偏移，指令选择只需要判断它能不能塞进 11 位。

### 为什么 sign 是独立一位

`zextloadi8` 和 `sextloadi8` 在 IR 层是两个不同节点，在 GM32 上是两条
不同指令。把 sign 编进 opcode 而不是让软件随后再扩展，等于把"这条加载
会不会污染高位"写进了机器码——模拟器不需要猜，反汇编器也能原样打印。

## 设计

### 指令表

| 空间 | load | store |
| --- | --- | --- |
| global | `b32` / `u8` / `s8` / `u16` / `s16` | `b32` / `b8` / `b16` |
| shared | 同上 | 同上 |
| local | 同上 | 同上 |
| constant | 同上 | —（只读） |

共 4×5 + 3×3 = **29** 条。constant 没有 store：只读空间写进去没有语义。

每个组合都是独立的 `def`，由 multiclass 展开：

```tablegen
multiclass GPUMinMemLoads<string space, bits<2> sp> {
  def _B32 : GPUMinLdM<OPC_LD.Value, sp, 0b10, 0,
                       "ld."#space#".b32 $rd, ${imm}(${base})">;
  ...
}
defm LD_GLOBAL : GPUMinMemLoads<"global", 0b00>;
```

这样 `-gen-disassembler` 能直接按 32 位字匹配出 `ld.shared.u16`，解码器
里没有任何手写位测试；`-gen-asm-matcher` 也按完整 mnemonic 串匹配，点号
不是分隔符而是名字的一部分。

### 地址选择

两条 pattern，顺序即优先级：

```tablegen
def simm11imm : ImmLeaf<i32, [{ return isInt<11>(Imm); }]>;

def : Pat<(load GPR:$base), (LD_GLOBAL_B32 $base, 0)>;
def : Pat<(load (add GPR:$base, simm11imm:$off)),
          (LD_GLOBAL_B32 $base, simm11imm:$off)>;
```

`ImmLeaf` 是关键：pattern 匹配时 `$off` 是常量节点，谓词在生成的 matcher
里执行，所以超出 11 位的偏移**不会**被截断成另一个地址，而是匹配失败、
退回"先算地址寄存器"的路径。

实测边界：

```llvm
; gep i32 %p, 256  -> 字节偏移 1024，塞不进 imm11
movi r1, 1024
add  r1, r4, r1
ld.global.b32 r4, 0(r1)
```

而 `gep i32 %p, 2`（8 字节）直接折叠：

```text
ld.global.b32 r4, 8(r4)
```

地址空间不参与 pattern 匹配：`load` 的 MemSDNode 带 AS，但生成的 matcher
对 AS 不做检查，所以 `addrspace(1)` 和 generic `ptr` 都会落到
`LD_GLOBAL_B32`。这正是本章想要的——generic 指针暂时按 global 处理，
第 16 章再用独立的 lowering 把 `addrspace(2)` 分出去。

### 已知边界

`movi` 只有 16 位立即数，所以"退回寄存器加法"这条路也只覆盖 ±32768：

```text
$ llc -march=gpumin huge.ll
LLVM ERROR: Cannot select: t3: i32 = Constant<400000>
```

这是**故意保留**的缺口：真正的 32 位符号地址需要 `movi.lo` / `movi.hi`
两条指令加一对重定位，属于第 07/08 章的活。本章把边界写进文档，而不是
用一个截断的编码把错误藏起来。

## 实现

### 指令类

```tablegen
class GPUMinLdM<bits<6> op, bits<2> space, bits<2> size, bit sign,
                string asmstr>
    : GPUMinInst<(outs GPR:$rd), (ins GPR:$base, simm11:$imm), asmstr, []> {
  bits<5> rd;
  bits<5> base;
  bits<11> imm;
  let Inst{31-26} = op;
  let Inst{25-21} = rd;
  let Inst{20-16} = base;
  let Inst{15-5} = imm;
  let Inst{4-3} = space;
  let Inst{2-1} = size;
  let Inst{0} = sign;
  let mayLoad = 1;
}
```

`mayLoad`/`mayStore` 不是装饰：调度器、机器 CSE、别名分析都靠它判断一条
指令有没有内存副作用。store 类没有 `rd` 输出，`rs2` 仍然占 `[25:21]`，
和 I 格式时代一致。

### 操作数与解析

```tablegen
def SImm11AsmOperand : GPUMinImmAsmOperand<"SImm11", "isSImm11">;
def simm11 : Operand<i32> {
  let ParserMatchClass = SImm11AsmOperand;
  let DecoderMethod = "DecodeSImm11";
}
```

parser 侧只加一个方法，规则和 `isSImm16` 一样：常量当场查范围，符号推迟
到 fixup。

```cpp
bool isSImm11() const {
  if (Kind != Immediate)
    return false;
  if (auto *CE = dyn_cast<MCConstantExpr>(Imm))
    return isInt<11>(CE->getValue());
  return true;
}
```

### 反汇编器

```cpp
static DecodeStatus DecodeSImm11(MCInst &Inst, uint64_t Imm, int64_t,
                                 const MCDisassembler *) {
  Inst.addOperand(MCOperand::createImm(SignExtend64<11>(Imm)));
  return MCDisassembler::Success;
}
```

少了 `SignExtend64<11>`，`ld.global.s8 r1, -4(r2)` 会打印成 `2044(r2)`。
编码是正确的，错的是"这个字段有符号"这条 ISA 语义——它只能由目标自己
声明，TableGen 生成不出来。

### include 顺序

`GPUMinInstrFormats.td` 里现在用到 `simm11`，而 `simm11` 定义在
`GPUMinInstrInfo.td` 的操作数区。TableGen 按定义顺序解析名字，所以
`include` 从文件头挪到了操作数类之后：

```tablegen
// The format classes are included here, after the operand classes: their
// operand lists name `simm11`, and TableGen resolves names in definition order.
include "GPUMinInstrFormats.td"
```

这不是 LLVM 特有规则，但报错信息只说 `Variable not defined: 'simm11'`，
第一次遇到很容易去查拼写而不是查顺序。

### 回改旧章节

第 01/02 章的测试和 Python 编码器都写死了 I 格式。本章同步修改：

- `check-ch01.sh`：两条编码期望值 + 越界测试（`2000(r2)` 现在必须被拒）；
- `check-ch02.sh`：round-trip 语料换成 6 条 M 格式访存；
- `tools/gpumin-encode.py`：新增 `word_m()` 与 `ld.<space>.<kind>` 解析，
  self-test 从 12 条扩到 16 条。

## 代码走读

### 一条 IR 走到机器码

```llvm
define i32 @folded(ptr addrspace(1) %p) {
  %q = getelementptr i32, ptr addrspace(1) %p, i32 2
  %x = load volatile i32, ptr addrspace(1) %q
  ret i32 %x
}
```

```text
$ llc -march=gpumin memory.ll -o -
folded:
        ld.global.b32 r4, 8(r4)
        exit
```

`gep +2` 在 DAG 里是 `add %p, 8`；常量 8 落在 imm11 范围内，直接被
`(load (add GPR, simm11imm))` 吃掉，没有任何多余的 `mov`。

### 逐位拆一条编码

```text
$ llvm-mc -triple=gpumin -show-encoding
ld.global.b32 r7, 8(r8)          # encoding: [0x04,0x01,0xe8,0x20]
```

小端读回 `0x20E80104`：

| 字段 | 值 | 贡献 |
| --- | --- | --- |
| opcode | 0x08 | `0x20000000` |
| rd = 7 | `0b00111` | `0x00E00000` |
| base = 8 | `0b01000` | `0x00080000` |
| imm11 = 8 | `8 << 5` | `0x00000100` |
| space = 0 | — | `0` |
| size = b32 = 2 | `2 << 1` | `0x00000004` |
| sign = 0 | — | `0` |

合计 `0x20E80104`。把 rd 换成 9、base 换成 10、位移换成 4，就得到
store 的 `0x252A0084`——`st.global.b32 r9, 4(r10)`。

### 寄存器索引地址

```llvm
%q = getelementptr i32, ptr addrspace(1) %p, i32 %i
```

```text
movi r1, 2
shl  r1, r5, r1
add  r1, r4, r1
ld.global.b32 r4, 0(r1)
```

`movi 2` 是"log2(4)"——GEP 的缩放因子；`shl` 完成 `i * 4`；`add` 之后
地址已经在寄存器里，访存用 0 位移。这是本章和"常量折叠"路径的分界：
能进 11 位的偏移不占寄存器，进不去的走 ALU。

## 验证

```text
$ llvm/lib/Target/GPUMin/scripts/check-ch06.sh <build>/bin
PASS 1/4: space[4:3], size[2:1] and sign[0] encode independently
PASS 2/4: the displacement is a signed byte offset, not an element index
PASS 3/4: encoder agreement and the asm -> obj -> asm identity
PASS 4/4: addrspace(1) loads/stores select, fold and round-trip
OK: chapter 06 gate passed
```

四组分别证明：

1. **字段独立**：global/shared/local/constant 只改 `[4:3]`，u8/s8 只差
   `bit 0`，b8/b16/b32 只差 `[2:1]`；
2. **位移语义**：±1024 边界通过、±1025 被拒；`b32` 与 `b8` 在同一字节
   偏移下的机器字只差 size 位，证明没有按元素缩放；
3. **三方一致**：11 条语料的 llvm-mc 字节 = Python 编码器字节，且
   `asm -> obj -> objdump` 回到原文；
4. **代码生成**：`addrspace(1)` 选中 global 形式，常量 GEP 折成 8 字节，
   索引 GEP 走 `shl + add + 0(base)`，且 codegen 的 `.o` 与"汇编 `.s` 再
   汇编"的 `.o` 逐字节一致。

第 00–05 章的门禁在本章改动后仍然全绿（`ld/st` 语料已同步更新）。

## 常见坑

1. **TableGen 名字解析是单向的**。类和操作数分居两个文件时，谁先 include
   谁就决定了能不能解析；报错是 `Variable not defined`，不是"顺序错误"。
2. **`ImmLeaf` 只在 pattern 左侧生效**。把 `simm11imm` 只写在结果里，生成
   的 matcher 不会做范围检查，超范围常量会被截断成错误地址。必须让它出现
   在 `(load (add ...))` 这一侧。
3. **load 没有 `.b8`**。8 位加载必须说清扩展方式（`u8`/`s8`）；`b8` 只出现
   在 store 上。写 `ld.global.b8` 会得到 `unrecognized instruction mnemonic`。
4. **位移是字节，GEP 已经乘过**。`gep i32, %p, 1` 的偏移是 4，不是 1；在
   编码测试里手写 `1(r2)` 是合法指令，但和 `gep +1` 不对应。
5. **mnemonic 里的点属于名字**。`ld.global.b32` 是一个完整 token，AsmString
   必须整串写出；拆成 `ld.` + 空间操作数会让 matcher 找不到指令。
6. **`Cannot select: Constant<400000>` 不是编码器 bug**。它是 `movi` 只有
   16 位的直接后果；先检查常量是否超过 ±32768，再谈 LO16/HI16。
7. **非 volatile 的 store 会被前向替换**。`store %v; load` 在 DAG 里可能
   直接变成 `mov`，测试里就看不到 `st.global.b32`。要观察两条指令，加
   `volatile`。
8. **objdump 的行首是 tab**。做文本 diff 前先规范化空白，否则每一行都
   "不相等"。

## 练习

1. 把 `ld.global.u16` 接进 ISel：需要先让 `i16` 成为合法类型，还是可以
   用 `zextloadi16` 直接匹配？两种做法分别缺什么？
2. 给 `st.global.b64` 设计编码：M 格式只剩 3 个 size 值，是扩 size 字段、
   还是引入第二条指令把 64 位拆成两次 32 位 store？写出你的取舍。
3. 把 `space` 参与 pattern 匹配：怎样让 `addrspace(2)` 的 `load` 选中
   `LD_SHARED_B32`，而不影响 generic 指针？（提示：`PatFrag` 的
   `AddressSpaces` 字段。）
4. 让 `1024(r2)` 不再报错：在 `movi + add` 之外，还有什么指令序列能用
   一条指令覆盖更大的偏移？它需要几位、代价是什么？

## 下一章

第 07 章：调用与 private stack。`call/ret`、`sp/lr`、frame lowering，以及
本章留下的 32 位地址物化——`movi.lo` / `movi.hi` 与一对重定位，正好和
调用约定的寄存器保存一起讲。
