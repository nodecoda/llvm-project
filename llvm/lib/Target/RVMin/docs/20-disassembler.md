# 第 20 章：反汇编器

## 目标

前 19 章一直在造"汇编 → 对象"的单向管道。本章补上反方向：让
`llvm-mc -disassemble` 与 `llvm-objdump -d --triple=rvmin` 使用 RVMin 自己的
解码器，形成 `asm → obj → disasm → asm` 的闭环。

完成本章后：

1. `llvm-mc -triple=rvmin -disassemble` 能解码 RV32I 与（带 `-mattr=+m` 的）M 指令；
2. `llvm-objdump --triple=rvmin -d` 能反汇编 RVMin 生成的对象与可执行文件；
3. 97 条编码语料（85 条 RV32I + 12 条 M）与 6 条标签分支，
   `asm → obj → disasm → asm` 的 `.text` 逐字节一致；
4. 负数立即数、B/J 型分散位偏移都能打印成正确的字节偏移；
5. 非法编码与不足 4 字节的尾部不会被"猜"成指令。

门禁脚本：`scripts/check-rvmin-disasm.sh`。

## 背景：哪些是生成的，哪些是手写的

TableGen 的 `-gen-disassembler` 从**同一份** `Inst{}` 位域定义生成解码表，
所以解码器与第 2 章的编码器天然对称：

```text
RVMinInstrInfo.td / RVMinInstrFormats.td
        │
        ├── -gen-emitter ──────────► RVMinMCCodeEmitter.inc   （第 2 章）
        └── -gen-disassembler ─────► RVMinGenDisassemblerTables.inc （本章）
```

生成的部分包括：opcode/funct3/funct7 的树状匹配、每条指令的操作数顺序、
`ECALL/EBREAK` 这种整字固定的编码。**操作数语义**必须手写，因为
TableGen 不知道"这 12 位是有符号数"：

| 操作数 | 编码位置 | 目标值 | 需要的解码方法 |
| --- | --- | --- | --- |
| `simm12` | I 型 imm[11:0] / S 型两段 | 有符号 12 位 | `decodeSImmOperand<12>` |
| `simm13_lsb0` | B 型分散位 | 有符号 13 位字节偏移 | `decodeSImmBranchOperand<13>` |
| `simm21_lsb0` | J 型分散位 | 有符号 21 位字节偏移 | `decodeSImmBranchOperand<21>` |
| `uimm5` / `uimm20` | 移位量 / U 型高 20 位 | 无符号 | 无需（TableGen 默认即可） |
| `GPR` | rs1/rs2/rd | `X0..X31` | `DecodeGPRRegisterClass` |

本项目在 B/J 型里存的是**完整字节偏移**（`RVMinInstrFormats.td` 开头的说明），
不是上游 RISC-V 的"预移位"值，所以解码时不需要再 `<<1`：TableGen 已经把
`imm13{12|10:5|4:1|11}` 重新拼成 13 位，剩下的只是符号扩展。

## 代码地图

| 位置 | 职责 |
| --- | --- |
| `CMakeLists.txt:9` | `llvm_tablegen(RVMinGenDisassemblerTables.inc -gen-disassembler)` |
| `CMakeLists.txt:51` | `add_subdirectory(Disassembler)`（出现这个目录，CMake 才会把 RVMin 写进生成的 `Disassemblers.def`） |
| `Disassembler/CMakeLists.txt` | `LLVMRVMinDisassembler` 组件（MC + MCDisassembler + RVMinInfo） |
| `Disassembler/RVMinDisassembler.cpp:51-57` | `RVMinDisassembler` 类与 `createRVMinDisassembler` |
| `Disassembler/RVMinDisassembler.cpp:62-73` | `LLVMInitializeRVMinDisassembler` 注册 |
| `Disassembler/RVMinDisassembler.cpp:77-92` | `GPRDecoderTable` + `DecodeGPRRegisterClass` |
| `Disassembler/RVMinDisassembler.cpp:94-114` | 符号扩展与分支目标解码 |
| `Disassembler/RVMinDisassembler.cpp:118` | `#include "RVMinGenDisassemblerTables.inc"` |
| `Disassembler/RVMinDisassembler.cpp:120-138` | `getInstruction`：固定 4 字节、失败也推进 4 字节 |
| `RVMinInstrInfo.td:50-53,87-103` | 三个 `DecoderMethod` 声明 |
| `llvm/include/llvm/Support/TargetSelect.h:118` | `InitializeAllDisassemblers` 遍历 `Disassemblers.def` |
| `scripts/check-rvmin-disasm.sh` | 本章门禁：97 条语料 + 6 条标签分支的字节级往返 |
| `llvm/test/MC/RVMin/disassembler.s` | lit：逐格式解码 + 无 `+m` 拒绝 + 非法/截断输入 |

## 关键实现

### 1. 注册：靠的是"目录存在"，不是手改工具

`llvm/CMakeLists.txt:1193` 在配置阶段做：

```cmake
if( EXISTS ${td}/Disassembler/CMakeLists.txt )
  set(LLVM_ENUM_DISASSEMBLERS
    "${LLVM_ENUM_DISASSEMBLERS}LLVM_DISASSEMBLER(${t})\n")
endif()
```

于是 `include/llvm/Config/Disassemblers.def` 多出一行 `LLVM_DISASSEMBLER(RVMin)`，
`InitializeAllDisassemblers()` 就会调用：

```cpp
extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializeRVMinDisassembler() {
  TargetRegistry::RegisterMCDisassembler(getTheRVMinTarget(),
                                         createRVMinDisassembler);
}
```

`llvm-mc` 与 `llvm-objdump` 都在 `main` 里调用 `InitializeAllDisassemblers()`
（`llvm-mc.cpp:391`、`llvm-objdump.cpp:4036`），所以不需要改任何工具代码。
这也是为什么**只加源文件、不加 `add_subdirectory`** 会得到
`no disassembler for target rvmin`：目录不存在时 CMake 根本不会生成那一行。

### 2. 寄存器：一张 32 项的查表

```cpp
static const unsigned GPRDecoderTable[] = { RVMin::X0, ..., RVMin::X31 };

static DecodeStatus DecodeGPRRegisterClass(MCInst &Inst, uint64_t RegNo,
                                           uint64_t Address,
                                           const MCDisassembler *Decoder) {
  if (RegNo >= std::size(GPRDecoderTable))
    return MCDisassembler::Fail;
  Inst.addOperand(MCOperand::createReg(GPRDecoderTable[RegNo]));
  return MCDisassembler::Success;
}
```

5 位字段天然落在 `0..31`，越界判断只是防御；真正的错误编码会被
`OPC_CheckField` 在进入这个函数之前挡掉。

### 3. 立即数：符号扩展是唯一的"手写语义"

```cpp
template <unsigned N>
static DecodeStatus decodeSImmOperand(MCInst &Inst, uint64_t Imm,
                                      int64_t Address,
                                      const MCDisassembler *Decoder) {
  Inst.addOperand(MCOperand::createImm(SignExtend64<N>(Imm)));
  return MCDisassembler::Success;
}
```

没有它，`addi a0, a0, -1`（编码 `0xfff`）会打印成 `addi a0, a0, 4095`，
再汇编回去直接报"immediate must be an integer in the range [-2048, 2047]"。

分支/跳转用同一套符号扩展，但把目标交给符号化器，好让 `llvm-objdump` 在
有符号表时打印标签：

```cpp
const int64_t Value = SignExtend64<N>(Imm);
if (!Decoder->tryAddingSymbolicOperand(Inst, Value, Address,
                                       /*IsBranch=*/true, /*Offset=*/0,
                                       /*OpSize=*/4, /*InstSize=*/4))
  Inst.addOperand(MCOperand::createImm(Value));
```

### 4. 固定 4 字节：失败也要推进一个字

```cpp
if (Bytes.size() < 4)
  return MCDisassembler::Fail;       // Size 保持 0：尾部不足一条指令

uint64_t Insn = support::endian::read32le(Bytes.data());
DecodeStatus Result =
    decodeInstruction(DecoderTable32, MI, Insn, Address, this, STI);
Size = 4;                            // 无论成功失败都消耗 4 字节
return Result;
```

`llvm-objdump` 在 `Fail` 时按 `Size` 前进；如果维持 `Size = 0`，它会对同一个
非法字按字节重试，打印四行 `<unknown>`（第一版就是这样）。RVMin 没有 C 扩展，
"一个 32 位字 = 一条候选指令"是明确不变的，所以失败时也推进 4 字节。

## 构建与验证

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop
ninja -C "$B" llc llvm-mc llvm-objdump llvm-readelf llvm-objcopy FileCheck

# 1. 本章 lit（解码 + 负例）
"$B/bin/llvm-lit" -v "$W/llvm/test/MC/RVMin/disassembler.s"

# 2. 97 条语料 + 6 条标签分支的字节级往返
"$W/llvm/lib/Target/RVMin/scripts/check-rvmin-disasm.sh" "$B/bin"

# 3. 全部门禁（现在是 6 项）
"$W/llvm/lib/Target/RVMin/scripts/rvmin-verify-all.sh" "$B/bin"
```

手工看一眼：

```bash
printf '0x13 0x05 0x10 0x00\n0x73 0x00 0x00 0x00\n' |
  "$B/bin/llvm-mc" -triple=rvmin -disassemble
# 	addi a0, zero, 1
# 	ecall

"$B/bin/llvm-objdump" --triple=rvmin -d demo.o | head
```

## 预期输出

门禁脚本：

```text
PASS: rv32i (85 instructions, .text round-trips byte-for-byte)
PASS: m (12 instructions, .text round-trips byte-for-byte)
PASS: labels (6 instructions, .text round-trips byte-for-byte)
PASS: 97 encoding-corpus instructions + 6 label-resolved branches/jumps
```

`llvm-objdump`（注意负数偏移和两条 `addi` 的符号扩展）：

```text
00000000 <fib>:
       0: 13 01 01 ff  	addi sp, sp, -16
       4: 23 26 11 00  	sw ra, 12(sp)
      1c: 63 46 b5 02  	blt a0, a1, 44
      2c: 13 05 f9 ff  	addi a0, s2, -1
      44: e3 e2 24 ff  	bltu s1, s2, -28
```

`rvmin-verify-all.sh` 现在是 6 项，第 4 项是本章新增：

```text
PASS 4/6: disassembler
```

## 常见坑

### 坑 1：解码表叫 `DecoderTable32`，不是 `DecoderTableRVMin32`

表名是 `"DecoderTable" + DecoderNamespace + BitWidth`，而 `DecoderNamespace`
默认是空串（`llvm/include/llvm/Target/Target.td:551`）。上游 RISCV 只有
`RV32Only` 这类"第二个解码器"才带名字。想确认就直接 grep 生成文件：

```bash
grep -n "static const uint8_t DecoderTable" \
  "$B/lib/Target/RVMin/RVMinGenDisassemblerTables.inc"
```

### 坑 2：忘了 `DecoderMethod`，往返测试会以"文本不同"暴露

`4095` vs `-1` 不是打印器的问题，而是解码器把 12 位当成了无符号数。
三个有符号操作数（`simm12`、`simm13_lsb0`、`simm21_lsb0`）都必须声明
`DecoderMethod`；只改打印器会掩盖错误的 MC 操作数。

### 坑 3：`Size = 0` 让 objdump 按字节刷屏

一个非法字打印四行 `<unknown>`，很容易误以为"解码表坏了"。固定长度 ISA
应该在失败时也返回 4；只有"尾部不足 4 字节"才保持 `Size = 0`。

### 坑 4：`llvm-objdump` 不加 `--triple` 会借 RISCV 的解码器

RVMin 对象的 ELF `e_machine` 是 `RISC-V`（第 2 章的选择，为了 `ld.lld` 能链接），
所以 `llvm-objdump -d foo.o` 会按 `riscv` 目标选择 RISCV 的解码器——输出看起来
"对"，但根本没走本章代码。门禁必须显式 `--triple=rvmin`。

### 坑 5：M 指令在 objdump 里也要 `--mattr=+m`

RVMin 不发射 `.riscv.attributes`，objdump 无法从 ELF 推断特性；默认特性下
解码表的 `HasStdExtM` 谓词不成立，`mul` 会变成 `<unknown>`。这和汇编器
需要 `-mattr=+m` 是同一个来源。

### 坑 6：`fieldFromInstruction` 在 `llvm::MCD` 命名空间

生成的表里直接写 `fieldFromInstruction(...)`，源文件需要
`using namespace llvm::MCD;`，否则编译报"not declared in this scope"。

## 练习

1. 给 `llvm-mc -disassemble` 增加一个 `--mattr=+m` 的负例测试：同一段 M 编码在
   默认特性下必须全部报 `invalid instruction encoding`。
2. 把 `check-rvmin-disasm.sh` 扩展成随机指令序列：随机 32 位字先查
   `llvm-mc -disassemble` 是否成功，再要求成功者能汇编回原字。
3. 用 `llvm-objdump --triple=rvmin --print-imm-hex -d` 对比十进制输出，确认
   `printOperand` 的符号处理。
4. 构造一个只有 2 字节 `.text` 的对象，观察 `llvm-objdump` 与 `llvm-mc` 的
   诊断差异，并解释 `Size` 的契约。
5. 给 `ECALL/EBREAK` 增加一个别名打印（例如 `scall`），只改打印器，验证
   编码与解码都不受影响。

## 下一章

第 21 章实现 Zbb/Zbs 位操作扩展：`clz`、`ctz`、`cpop`、`rol/ror`、`andn/orc`、
`rev8` 等指令与 pattern，并保持默认（无扩展）输出不变。
