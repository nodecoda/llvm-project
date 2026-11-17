# 第 43 章：RV32C 的 MC 层

## 目标

前 42 章产出的每个指令都是 32 位、4 字节边界对齐。本章把 **RV32C（压缩
指令）**接进 MC 层，完成之后：

1. 35 条 `c.*` 指令可以被 `llvm-mc -triple=rvmin -mattr=+c,+f,+d` 汇编，
   编码与参考 `llvm-mc -triple=riscv32 -mattr=+c,+f,+d` **逐字节相同**；
2. 16 位指令的 `.text` 字节流可以反汇编、可以重汇编，且 `.text` 不变；
3. 16/32 位混排时反汇编器逐条按 `Size` 推进，不会把压缩指令后面的 32 位
   指令吞掉；
4. `c.beqz/c.bnez/c.j/c.jal` 的标签由专用 fixup 解析，越界与奇地址（非 2
   字节对齐）报错；
5. 缺 `+c` 报 `'C' (Compressed)`；`c.flw/c.fld` 分别要求 `+f/+d`；
6. `clang -march=rv32ic` 与 `-march=rv32imafdc` 端到端可用；
   `-march=rv32gc` 仍被明确拒绝（Zicsr 在第 46 章）；
7. 不开 `+c` 时，前 42 章的输出逐字节不变。

本章**只做 MC 层**：汇编器接受显式 `c.*`，但 `llc` 还不会自动把 32 位指令
压缩。压缩代码生成（`CompressPat` + `-gen-compress-inst-emitter`）是第 44 章。

## 背景：16 位指令为什么是另一套格式

RV32C 不是“把指令变短”，而是一组独立的 16 位编码格式。本章要处理五件事：

| 维度 | 32 位指令（前 42 章） | 16 位压缩指令（本章） |
| --- | --- | --- |
| 指令字 | `bits<32> Inst`，`Size = 4` | `bits<16> Inst`，`Size = 2` |
| 格式 | R/I/S/B/U/J/R4 | CR/CI/CSS/CIW/CL/CS/CB/CJ（+CA 扩展） |
| 寄存器 | 5 位，任意 x0–x31 | 3 位，`x8–x15`（`GPRC`）；FP 只能 `f8–f15` |
| 立即数 | 连续字段 | 打散在字内，位序由规范固定 |
| 解码入口 | 固定读 4 字节 | 先读 2 字节，看 `bits[1:0]`：`!= 0b11` 才是压缩指令 |

`bits[1:0] != 0b11` 是 RISC-V 的“16 位判别位”。RVC 的每条指令都保证低两位
不是 `11`，所以解码器只需要一次分支就能选表；32 位指令的低两位恒为 `11`。
这也是 `c.*` 与 32 位指令不可能混淆的原因。

另一个容易忽略的点是**对齐与填充**：`.text` 里可以出现 2 字节指令，但函数
与段仍然按 4 字节对齐。`writeNopData` 必须能在只剩 2 字节时写一条 `c.nop`
（`0x0001`），而不是硬写 4 字节的 `addi x0, x0, 0`。

## 代码地图

| 文件 | 本章改动 |
| --- | --- |
| `llvm/lib/Target/RVMin/RVMinInstrInfoC.td` | **新增**：9 个立即数操作数、9 个格式基类、35 条指令 |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td` | 新增 `FmtCR..FmtCA`（8–16） |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td` | 新增 `GPRC`（x8–x15）、`SP`（仅 x2）、`FPR32C`、`FPR64C` |
| `llvm/lib/Target/RVMin/RVMin.td` | `FeatureStdExtC`/`HasStdExtC`、`generic-rv32-c`、include |
| `llvm/lib/Target/RVMin/RVMinSubtarget.h` | `hasStdExtC()` |
| `AsmParser/RVMinAsmParser.cpp` | 9 个立即数谓词（范围 + 对齐 + 非零） |
| `MCTargetDesc/RVMinMCCodeEmitter.cpp` | 按 `MCII.get(Opcode).getSize()` 发射 2/4 字节；RVC fixup 分类 |
| `MCTargetDesc/RVMinFixupKinds.h` | `rvc_branch` / `rvc_jump` / `rvc_imm` |
| `MCTargetDesc/RVMinAsmBackend.cpp` | `encodeCBImm`/`encodeCJImm`、`Infos`、`applyFixup` 16 位读写、`c.nop` 填充 |
| `MCTargetDesc/RVMinELFObjectWriter.cpp` | `R_RISCV_RVC_BRANCH/JUMP` 映射 |
| `Disassembler/RVMinDisassembler.cpp` | 16 位表选择、`GPRC`/`FPRC`/`SP` 解码器 |
| `clang/lib/Basic/Targets/RVMin.cpp` | `c/zca/zcf/zcd` 白名单、`generic-rv32-c` |
| `clang/lib/Driver/ToolChains/Arch/RISCV.cpp` | `+c` 保留、CPU 映射、临时拒绝 `g` |
| `llvm/test/MC/RVMin/c-extension.s` | **新增**：35 条编码 + 反汇编 + 标签 + 诊断 |
| `scripts/rvmin-c-encoding-corpus.s` | **新增**：35 行编码语料 |
| `scripts/rvmin-mixed-encoding-corpus.s` | **新增**：16/32 混排语料 |
| `scripts/check-rvmin-encodings.sh` | 新增 C 段（35 条对照） |
| `scripts/check-rvmin-disasm.sh` | 新增 2 字节分组往返 + 混排往返 |

## 关键实现

### 1. 指令基类与格式

32 位基类固定 `field bits<32> Inst; let Size = 4;`。压缩指令需要一个平行的
基类：

```tablegen
class RVMinCInst<bits<16> inst, dag outs, dag ins, string asmstr,
                 string opstr> : Instruction {
  bits<16> Inst;
  let Size = 2;
  let Inst = inst;
  ...
}
```

九个格式类各自负责 `op`/`funct3`/`funct4` 的固定位，例如 CB 型：

```tablegen
class RVMinCBInst<bits<3> funct3, bits<2> op, dag outs, dag ins,
                  string asmstr, string opstr>
    : RVMinCInst<0, outs, ins, asmstr, opstr> {
  let Inst{15-13} = funct3;
  let Inst{1-0} = op;
}
```

35 条指令分四组：整数 27 条、F 4 条、D 4 条。FP 变体在 `Predicates` 里追加
`HasStdExtF` / `HasStdExtD`，所以缺特性时会得到精确诊断，而不是“未知指令”。

### 2. 立即数：范围、对齐、非零

本章新增的操作数（`RVMinInstrInfoC.td`）：

| 操作数 | 位宽/对齐 | 用途 |
| --- | --- | --- |
| `simm6` | 6 位有符号 | `c.addi/c.li/c.andi` |
| `uimm5` | 5 位 | `c.slli/c.srli/c.srai` |
| `uimm7_lsb00` | 7 位、4 对齐 | `c.lw/c.sw/c.flw/c.fsw` |
| `uimm8_lsb00` | 8 位、4 对齐 | `c.lwsp/c.swsp/c.flwsp/c.fswsp` |
| `uimm8_lsb000` | 8 位、8 对齐 | `c.fld/c.fsd` |
| `uimm9_lsb000` | 9 位、8 对齐 | `c.fldsp/c.fsdsp` |
| `uimm10_lsb00nonzero` | 10 位、4 对齐、非零 | `c.addi4spn` |
| `simm10_lsb0000nonzero` | 10 位、16 对齐、非零 | `c.addi16sp` |
| `simm9_lsb0` | 9 位、2 对齐 | `c.beqz/c.bnez` |
| `simm12_lsb0` | 12 位、2 对齐 | `c.j/c.jal` |
| `c_lui_imm` | 20 位宽形式：1–31 或 0xfffe0–0xfffff | `c.lui`（第 44 章改为宽形式） |

“非零”不是洁癖：`c.addi4spn` 的立即数为 0 时整条指令是保留编码；
`c.addi16sp` 的立即数为 0 时与 `c.nop` 的位型冲突。把它们做成操作数谓词，
错误在解析期就暴露，而不是留到 fixup。

### 3. 位序：CB 与 CJ 是最容易写错的地方

CB 型分支的偏移位（`simm9`，单位字节）在字内是：

| 指令位 | 12 | 11–10 | 6–5 | 4–3 | 2 |
| --- | --- | --- | --- | --- | --- |
| 偏移位 | 8 | 4–3 | 7–6 | 2–1 | 5 |

CJ 型跳转（`simm12`）：

| 指令位 | 12 | 11 | 10–9 | 8 | 7 | 6 | 5–3 | 2 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 偏移位 | 11 | 4 | 9–8 | 10 | 6 | 7 | 3–1 | 5 |

这些位序在 `RVMinInstrInfoC.td` 里逐位写出，在 `RVMinAsmBackend.cpp` 里由
`encodeCBImm`/`encodeCJImm` 反向拼装。两个方向必须成对修改——本章用 35 行
语料 + 标签语料同时压这两条路径。

### 4. 编码器：按 `Size` 发射

`RVMinMCCodeEmitter::encodeInstruction` 不再固定写 4 字节：

```cpp
unsigned Size = MCII.get(Opcode).getSize();
for (unsigned i = 0; i != Size; ++i)
  support::endian::write<uint8_t>(CB, Inst >> (8 * i), Endian);
```

符号操作数的 fixup 分发也按 `Size` 区分：`c.beqz/c.bnez` 走 `rvc_branch`，
`c.j/c.jal` 走 `rvc_jump`，其余 2 字节指令遇到符号立即数走 `rvc_imm`
（该 fixup 只负责报错——压缩指令没有 `%hi/%lo` 这类重定位）。

### 5. fixup：16 位读写 + 对齐/范围检查

`RVMinAsmBackend.cpp` 新增：

- `Infos` 三项：`rvc_branch`（2 对齐、9 位）、`rvc_jump`（2 对齐、12 位）、
  `rvc_imm`（2 字节、报错）；
- `applyFixup` 三个 case：`read16le` / `write16le`，先检查目标地址 2 字节
  对齐，再检查偏移是否落在 `simm9`/`simm12` 范围；
- `writeNopData`：4 字节倍数仍写 `0x00000013`；否则若 2 字节对齐且开了 C，
  补 `0x0001`（`c.nop`）。

错误信息是 `compressed branch target out of range`，比一句笼统的
`fixup value out of range` 更容易定位。

### 6. 反汇编：先看低两位

`RVMinDisassembler::getInstruction` 的入口改成：

```cpp
uint16_t Half = read16le(Bytes);
if ((Half & 0x3) != 0x3) {
  // DecoderTable16，Size = 2
} else {
  // 现有 32 位路径，Size = 4
}
```

TableGen 从 `bits<16>` 自动生成 `DecoderTable16`，无需手写。

三个寄存器解码器：

- `DecodeGPRCRegisterClass`：8 项表，`field + 8`；
- `DecodeFPR32CRegisterClass` / `DecodeFPR64CRegisterClass`：同样 `+8`；
- `DecodeSPRegisterClass`：**两个重载**。`SP` 类只有 x2，但 TableGen 生成
  的解码调用有两种签名（2 参与 4 参），两个都要提供，否则链接期才报错。

### 7. clang：白名单、CPU、临时拒绝 `g`

- `clang/lib/Basic/Targets/RVMin.cpp`：`isRVMinExtension` 接受
  `c`、`zca`、`zcf`、`zcd`。**必须四个都接受**：`RISCVISAInfo` 会把 `c`
  展开成 `zca/zcf/zcd` 再做特性校验；
- `erase_if` 保留 `+c`；
- CPU 列表加 `generic-rv32-c`（映射 `rv32ic`）；
- driver 在 RVMin 分支开头拒绝含 `g` 的 `-march`：`g` 蕴含
  `zicsr/zifencei`，而本章还没有 CSR 指令。这个拒绝在第 46 章删除。

## 构建与验证

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop

ninja -C "$B" llc clang llvm-mc llvm-lit llvm-objdump

# 1) 本章 lit：35 条编码 + 反汇编 + 标签 + 诊断
"$B/bin/llvm-lit" -v "$W/llvm/test/MC/RVMin/c-extension.s"

# 2) 编码对照（35 行）
"$B/bin/llvm-mc" -triple=rvmin   -mattr=+c,+f,+d -show-encoding \
  "$W/llvm/lib/Target/RVMin/scripts/rvmin-c-encoding-corpus.s" > /tmp/rvmin-c.txt
"$B/bin/llvm-mc" -triple=riscv32 -mattr=+c,+f,+d -show-encoding \
  "$W/llvm/lib/Target/RVMin/scripts/rvmin-c-encoding-corpus.s" > /tmp/riscv-c.txt
diff <(grep -o 'encoding: \[[^]]*\]' /tmp/rvmin-c.txt) \
     <(grep -o 'encoding: \[[^]]*\]' /tmp/riscv-c.txt)

# 3) 反汇编 + 16/32 混排往返
bash "$W/llvm/lib/Target/RVMin/scripts/check-rvmin-disasm.sh" "$B/bin"

# 4) clang
"$B/bin/clang" --target=rvmin-unknown-elf -march=rv32ic -mabi=ilp32 -O2 -S \
  -x c - -o - <<< 'int f(int x) { return x + 1; }'
"$B/bin/clang" --target=rvmin-unknown-elf -march=rv32imafdc -mabi=ilp32d -O2 -S \
  -x c - -o - <<< 'float f(float x) { return x + 1.0f; }'
! "$B/bin/clang" --target=rvmin-unknown-elf -march=rv32gc -mabi=ilp32 -O2 -S \
  -x c - -o /dev/null <<< 'int f(int x) { return x + 1; }'

# 5) 8 项门禁
bash "$W/llvm/lib/Target/RVMin/scripts/rvmin-verify-all.sh" "$B/bin"
```

## 预期输出

```
$ llvm-lit c-extension.s
PASS: LLVM :: MC/RVMin/c-extension.s (1 of 1)

$ llvm-mc -triple=rvmin -mattr=+c,+f,+d -show-encoding rvmin-c-encoding-corpus.s
	c.nop                    # encoding: [0x01,0x00]
	c.addi a0, 1              # encoding: [0x05,0x05]
	c.li a0, -1               # encoding: [0x7d,0x55]
	c.addi16sp sp, 16         # encoding: [0x41,0x61]
	c.j 8                     # encoding: [0x21,0xa0]
	c.jal 8                   # encoding: [0x21,0x20]
	c.flw fa0, 8(a1)          # encoding: [0x88,0x65]
	c.fld fa0, 8(a1)          # encoding: [0x88,0x25]

$ bash check-rvmin-encodings.sh <bin>
PASS: 85 RV32I + 12 M + 19 Zbb + 8 Zbs + 20 A + 28 F + 28 D + 35 C instructions, .text identical

$ bash check-rvmin-disasm.sh <bin>
PASS: c (35 compressed instructions, .text round-trips byte-for-byte)
PASS: mixed (6 instructions, mixed 16/32-bit .text round-trips)
PASS: 173 encoding-corpus instructions + 35 compressed + 6 mixed + 6 label-resolved branches/jumps
```

`llvm-objdump -d` 的关键现象是地址每次加 2：

```
       0: 01 00        	c.nop
       2: 05 05        	c.addi a0, 1
       4: 7d 55        	c.li a0, -1
       ...
      2e: 01 c5        	c.beqz a0, 8
      30: 01 e5        	c.bnez a0, 8
      32: 21 a0        	c.j 8
```

## 常见坑

1. **`shamt` 不能声明成 `bits<5>` 再去取 bit 5。** `c.slli/c.srli/c.srai`
   的 bit 12 是固定的 0（RV32 的 shamt 只有 5 位）。写成
   `let Inst{12} = shamt{5}` 会生成越界访问，TableGen 直接拒绝。正确写法是
   `let Inst{12} = 0; let Inst{6-2} = shamt{4-0};`。
2. **`AsmString` 里的 `$imm($rs1)` 会被解析成名为 `imm(` 的操作数。**
   括号前必须写 `${imm}(${rs1})`。
3. **`SP` 寄存器类不要加 `DecoderMethod`。** 加了会生成一个 2 参调用，
   与手写的 4 参定义不匹配；但 TableGen 仍然会调用 `DecodeSPRegisterClass`，
   所以要在反汇编器里同时提供 2 参与 4 参两个重载。
4. **`RISCVISAInfo` 把 `c` 展开成 `zca/zcf/zcd`。** 白名单只加 `c` 会在
   driver 层被拒绝，必须四个一起接受。
5. **`std::string` 没有 `.contains()`（C++17）。** 判断 `g` 要用
   `StringRef(MArch).contains('g')`。
6. **不要用 4 字节分组去反汇编压缩语料。** `llvm-mc -disassemble` 按行解码，
   35 条 16 位指令必须按 2 字节一行喂进去；混排流则走
   `llvm-objdump → 文本 → 重汇编` 的路径。
7. **`.text` 的 2 字节尾巴。** 没有 `c.nop` 填充时，4 字节对齐的段会多写
   2 字节或直接失败；`writeNopData` 的这个分支在 C 关闭时必须保持原行为。

## 练习

1. 把 `c.jalr/c.jr` 的 `isCall/isBarrier/isTerminator` 标记去掉，观察
   `-verify-machineinstrs` 或后续章节的 CFG 是否还能通过；解释每个标记
   在 MC 层与 MI 层分别起什么作用。
2. 给 `c.lui` 加上“立即数不能为 0、不能等于 32”的约束（规范里这两者是
   保留/特殊编码），并用 lit 测试固定诊断文本。
3. 用 `llvm-mc -triple=riscv32 -mattr=+c` 生成 `c.addi4spn` 的 16 个
   非零立即数，验证 `uimm10_lsb00nonzero` 的每一位都落在正确位置。
4. 把 35 条语料扩成 64 条（覆盖 `x8`/`x15`、`f8`/`f15` 边界），确认
   `GPRC` 的 3 位编码与 `+8` 解码在边界上仍然一致。
5. 写一个最小脚本，把 `.text` 按低两位自动切成 2/4 字节后喂给
   `llvm-mc -disassemble`，替代本章“混排走 objdump”的路径。

## 产物清单

- 代码：`RVMinInstrInfoC.td`（新）、`RVMinInstrFormats.td`、`RVMinRegisterInfo.td`、
  `RVMin.td`、`RVMinSubtarget.h`、`RVMinAsmParser.cpp`、`RVMinMCCodeEmitter.cpp`、
  `RVMinFixupKinds.h`、`RVMinAsmBackend.cpp`、`RVMinELFObjectWriter.cpp`、
  `RVMinDisassembler.cpp`、`clang/lib/Basic/Targets/RVMin.cpp`、
  `clang/lib/Driver/ToolChains/Arch/RISCV.cpp`。
- 测试：`llvm/test/MC/RVMin/c-extension.s`。
- 门禁：`scripts/rvmin-c-encoding-corpus.s`、`scripts/rvmin-mixed-encoding-corpus.s`、
  `check-rvmin-encodings.sh`（C 段）、`check-rvmin-disasm.sh`（`roundtrip_c` +
  `roundtrip_mixed`）。
- tag：`rvmin-ch43`。
