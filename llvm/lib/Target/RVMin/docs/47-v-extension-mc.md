# 第 47 章：V 的 MC 层——从 `v0` 到 19 条向量指令

## 目标

从这一章开始，RVMin 进入向量世界。第一个目标不是代码生成，而是把 **MC 层**
做对：汇编器能编码、反汇编器能还原、诊断能说清缺哪个特性。

范围是 **Zve32x 教学子集**，不是完整 RVV：

| 维度 | 本章取值 |
| --- | --- |
| SEW | 32 |
| LMUL | 1 |
| VLEN | 128（固定，一个寄存器装 `<4 x i32>`） |
| 元素类型 | 整数 |
| mask | 无（`vm` 固定为 1） |
| 向量 FP | 无 |

首批 19 条指令：

- 配置：`vsetivli`、`vsetvli`、`vsetvl`
- 访存：`vle32.v`、`vse32.v`
- 搬移：`vmv.v.i`、`vmv.v.v`、`vmv.x.s`、`vmv.s.x`
- 算术：`vadd.vv/vx/vi`、`vsub.vv/vx`、`vmul.vv/vx`
- 逻辑：`vand.vv`、`vor.vv`、`vxor.vv`

本章**不改变代码生成**。没有任何 TableGen pattern 使用这些指令；`llc
-mattr=+v` 仍然生成第 46 章那样的标量代码。向量代码生成从第 48 章开始。

## 背景：为什么 V 先做 MC 层

V 的规模远大于前面所有扩展。完整 RVV 有几百条指令、mask、LMUL 1–8、
strided/indexed 访存、向量 FP、向量 ABI 和 VSETVLI 优化。直接写代码生成会同时
踩三类坑：编码位序、类型合法化、VL/VTYPE 状态。把 MC 层单独做成一章，可以先
用参考汇编器把每个 bit 钉死，再让代码生成只关心“选哪条指令”。

另一个原因是 `vset*`。向量指令的行为依赖当前的 VL 和 VTYPE；没有 `vset*`，
后面生成的代码根本跑不起来。所以这一章先把配置指令、向量寄存器和一个足够
覆盖 load/store/算术的指令集合打牢。

## 代码地图

| 位置 | 职责 |
| --- | --- |
| `RVMin.td` | `FeatureStdExtV`（隐含 Zicsr）、`HasStdExtV`、`generic-rv32-v`、include `RVMinInstrInfoV.td` |
| `RVMinSubtarget.h` | `HasStdExtV` 字段与 `hasStdExtV()` |
| `RVMinRegisterInfo.td` | `V0`–`V31`、`VR` 寄存器类（`v4i32`，128 位） |
| `RVMinInstrFormats.td` | `FmtV`（TSFlags 17）、`OPC_OP_V`/`OPC_VLOAD`/`OPC_VSTORE` |
| `RVMinInstrInfoV.td` | 19 条指令、`vtypei10/vtypei11`、`simm5`、V 型基类 |
| `AsmParser/RVMinAsmParser.cpp` | `parseVType`、`isVTypeI10/isVTypeI11`、`isSImm5`、`Match_InvalidVType` |
| `MCTargetDesc/RVMinInstPrinter.{h,cpp}` | `printVTypeI`：把编码还原成 `e32, m1, ta, ma` |
| `Disassembler/RVMinDisassembler.cpp` | `DecodeVRRegisterClass`（V0..V31 表） |
| `clang/lib/Basic/Targets/RVMin.cpp` | 接受 `v` 与它的 `zv*` 展开、`generic-rv32-v` |
| `clang/lib/Driver/ToolChains/Arch/RISCV.cpp` | 白名单保留 `+v`，丢弃 `zvl*` 等隐含特性 |
| `test/MC/RVMin/v-extension.s` | 编码、反汇编、CPU、缺特性与越界诊断 |
| `scripts/rvmin-v-encoding-corpus.s` | 19 条编码语料 |
| `scripts/check-rvmin-{encodings,disasm}.sh` | 新增 V 段与 `roundtrip v` |
| `scripts/rvmin-clang-matrix.sh` | 新增 `rv32imafdv`（432 对象） |

## 关键实现

### 1. 特性与寄存器：V 只隐含 Zicsr

上游 `FeatureStdExtV` 隐含 `Zvl128b` 和 `Zve64d`；RVMin 只实现 Zve32x，
不能照抄：

```tablegen
def FeatureStdExtV : SubtargetFeature<"v", "HasStdExtV", "true",
                                      "Vector extension (Zve32x subset)",
                                      [FeatureStdExtZicsr]>;
```

- 隐含 Zicsr 是因为 `vset*` 和 `vl/vtype/vlenb` 这些 CSR 属于 Zicsr；
- **不**隐含 F/D：否则 `rv32iv` 会被误判成需要浮点；
- VLEN=128 是文档承诺和寄存器类的大小，不额外引入 `Zvl128b` 特性。

向量寄存器 `v0`–`v31` 每个 5 位编码：

```tablegen
class RVMinVReg<bits<5> num, string n> : Register<n> {
  field bits<5> Num = num;
  let Namespace = "RVMin";
  let HWEncoding{4-0} = num;
}
```

寄存器类只有一个：

```tablegen
def VR : RegisterClass<"RVMin", [v4i32], 128,
  (add (sequence "V%u", 8, 31),
       (sequence "V%u", 0, 7))> {
  let DecoderMethod = "DecodeVRRegisterClass";
}
```

分配顺序从 `v8` 开始、最后才用 `v0`–`v7`：完整 RVV 里 `v0` 是 mask 寄存器，
虽然本章不用 mask，但保持上游顺序可以让第 48 章的分配结果更自然。

### 2. 19 条指令的编码

所有 V 指令都是 32 位。三条 `vset` 用 OP-V（`0x57`），向量 load/store 用
`0x07`/`0x27`，其余算术/搬移也用 OP-V。

| 指令 | 编码（小端字节） |
| --- | --- |
| `vsetivli zero, 4, e32, m1, ta, ma` | `57 70 02 cd` |
| `vsetvli a0, a1, e32, m1, ta, ma` | `57 f5 05 0d` |
| `vsetvl a0, a1, a2` | `57 f5 c5 80` |
| `vle32.v v8, (a0)` | `07 64 05 02` |
| `vse32.v v8, (a0)` | `27 64 05 02` |
| `vmv.v.i v8, 5` | `57 b4 02 5e` |
| `vmv.v.v v8, v9` | `57 84 04 5e` |
| `vmv.x.s a0, v8` | `57 25 80 42` |
| `vmv.s.x v8, a0` | `57 64 05 42` |
| `vadd.vv v8, v9, v10` | `57 04 95 02` |
| `vadd.vx v8, v9, a0` | `57 44 95 02` |
| `vadd.vi v8, v9, 5` | `57 b4 92 02` |
| `vsub.vv v8, v9, v10` | `57 04 95 0a` |
| `vsub.vx v8, v9, a0` | `57 44 95 0a` |
| `vmul.vv v8, v9, v10` | `57 24 95 96` |
| `vmul.vx v8, v9, a0` | `57 64 95 96` |
| `vand.vv v8, v9, v10` | `57 04 95 26` |
| `vor.vv v8, v9, v10` | `57 04 95 2a` |
| `vxor.vv v8, v9, v10` | `57 04 95 2e` |

OP-V 的通用布局：

```text
31      26 25 24    20 19    15 14  12 11     7 6        0
   funct6   vm     vs2       vs1      funct3    vd     1010111
```

`funct6`/`funct3` 的取值：

| 操作 | funct6 | `.vv` | `.vx` | `.vi` |
| --- | --- | --- | --- | --- |
| `vadd` | `000000` | OPIVV `000` | OPIVX `100` | OPIVI `011` |
| `vsub` | `000010` | OPIVV `000` | OPIVX `100` | — |
| `vmul` | `100101` | OPMVV `010` | OPMVX `110` | — |
| `vand` | `001001` | OPIVV `000` | — | — |
| `vor` | `001010` | OPIVV `000` | — | — |
| `vxor` | `001011` | OPIVV `000` | — | — |

注意 `vmul` 不在 OPIVV/OPIVX，而在 OPMVV/OPMVX：funct3 分别是 `010`/`110`。
这是最容易照抄错的一处。

### 3. vtype：一个文法，两种拼写

`vset*` 的核心是 8 位 vtype：

```text
vtype = vma << 7 | vta << 6 | vsew << 3 | vlmul
```

本章的 `e32, m1, ta, ma` 对应 `vsew=010`、`vlmul=000`、`vta=vma=1`，即
`0xD0`。三种指令对它的编码不同：

| 指令 | vtype 位置 | 宽度 | AVL |
| --- | --- | --- | --- |
| `vsetivli` | `Inst{29-20}` | 10 位 | `uimm5`（`Inst{19-15}`） |
| `vsetvli` | `Inst{30-20}` | 11 位 | `rs1` |
| `vsetvl` | `rs2` | GPR | `rs1` |

`vsetivli` 的 `vtypei[10]` 由 `Inst{31:30}=11` 隐含，所以汇编器只接收 10 位
数字（0..1023）；`vsetvli` 接收 11 位（0..2047）。

汇编器允许两种写法：

```asm
vsetivli zero, 4, e32, m1, ta, ma
vsetivli zero, 4, 0xd0
```

两者产生同一个 `MCInst`。`parseVType` 的处理分两路：

- 标识符：按 `SEW, LMUL, tail, mask` 顺序读四个 token，**自行吃掉逗号**，
  最后合成一个立即数操作数；
- 表达式：折叠成绝对值，检查 `0..2047`。

打印器走 `printVTypeI`：把 `0xD0` 解码回 `e32, m1, ta, ma`；如果 SEW/LMUL
落在保留编码（例如 `vlmul=100`），就退回打印数字，保证输出仍可重汇编。

### 4. 向量 load/store

`vle32.v`/`vse32.v` 是 unit-stride、无 mask、32 位元素：

```text
31  29 28 27 26 25 24    20 19    15 14 12 11     7 6      0
 nf=0 mew mop  vm  lumop/sumop  rs1   width  vd/vs3  opcode
      0   00   1      00000            110
```

`width=110` 表示 SEW=32。`nf=0` 表示只搬一个字段；`mop=00` 表示 unit-stride。
`vle32.v` 的 opcode 是 `0000111`，`vse32.v` 是 `0100111`——都不同于标量
load/store。

### 5. vmv 的固定字段

四个搬移指令共用 `funct6=010000`（`vmv.x.s`/`vmv.s.x`）或 `010111`
（`vmv.v.*`），但固定字段不同：

| 指令 | funct3 | vs2 | vs1 | 结果 |
| --- | --- | --- | --- | --- |
| `vmv.v.v` | OPIVV `000` | 0 | `vs1` | `vd = vs1` 广播 |
| `vmv.v.i` | OPIVI `011` | 0 | `simm5` | `vd = imm` 广播 |
| `vmv.x.s` | OPMVV `010` | `vs2` | 0 | `rd = vs2[0]` |
| `vmv.s.x` | OPMVX `110` | 0 | `rs1` | `vd[0] = rs1` |

`vmv.v.i` 的立即数是 **5 位有符号**，所以 `-16..15`；`vmv.s.x` 只写第 0 个
元素，其余元素保持不变（本章不展开该语义，代码生成阶段再处理）。

### 6. clang：`rv32imafdv` 可用，但只承诺子集

`RISCVISAInfo` 会把 `v` 展开成一组 `zve*`/`zvl*` 扩展。RVMin 的策略是：

- 前端接受 V 的 `Zve`/`Zvl` 隐含闭包（`zve32x/zve32f/zve64x/zve64f/
  zve64d/zvl32b/zvl64b/zvl128b`），但拒绝显式的其它 `Zv*`（例如 `_zvbb`）；
- 最终 target-features 只保留 `+v`（以及 `+zicsr`），因为后端只实现子集；
- `-march=rv32imafdv -mabi=ilp32d` 可以编译，`rv32imafdc`/`rv32gc` 不回归。

这不是“支持完整 V”的声明。文档和验收章都必须写明：`v` 代表 Zve32x 子集。

## 构建与验证

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop
ninja -C "$B" llc clang llvm-mc llvm-objdump llvm-readelf llvm-objcopy FileCheck
```

单条编码：

```console
$ llvm-mc -triple=rvmin -mattr=+v -show-encoding <<< 'vadd.vv v8, v9, v10'
        vadd.vv v8, v9, v10                     # encoding: [0x57,0x04,0x95,0x02]
```

反汇编：

```console
$ llvm-mc -triple=rvmin -mattr=+v -filetype=obj /tmp/v.s -o /tmp/v.o
$ llvm-objdump -d --triple=rvmin --mattr=+v /tmp/v.o
       0: 57 70 02 cd   vsetivli zero, 4, e32, m1, ta, ma
       4: 57 f5 05 0d   vsetvli a0, a1, e32, m1, ta, ma
       ...
      48: 57 04 95 2e   vxor.vv v8, v9, v10
```

编码门禁：

```text
PASS: 85 RV32I + 12 M + 19 Zbb + 8 Zbs + 19 A + 28 F + 28 D + 35 C + 24 Zicsr/Zifencei + 19 V instructions, .text identical
```

反汇编门禁：

```text
PASS: v (19 instructions, .text round-trips byte-for-byte)
PASS: 191 encoding-corpus instructions + 35 compressed + 6 mixed + 24 zicsr/zifencei + 6 label-resolved branches/jumps
```

lit（新增 `v-extension.s` 后共 32 个测试）：

```text
Total Discovered Tests: 32
  Passed: 32 (100.00%)
```

clang 矩阵（9 个 ISA × 4 个优化级 × 12 个程序）：

```text
PASS: e2e corpus under -march=rv32imafdv -O3
ALL CLANG MATRIX PASS: 432 objects, ELF32 EM_RISCV, -S/-c agree, .text matches the riscv32 TargetInfo baseline
```

八项门禁：

```text
PASS 3/8: encodings
PASS 4/8: disassembler
PASS 5/8: e2e
PASS 6/8: 204 llc runs with -verify-machineinstrs
PASS 7/8: TableGen DSL contracts + status matrix + field effect
PASS 8/8: QEMU run (RV32I/M/A + F/D float/double + compressed demo)
ALL 8 GATES PASS
```

## 验收表

| ID | 标准 | 证据 | 结果 |
| --- | --- | --- | --- |
| V-MC1 | 19 条 V 指令的编码与 `riscv32 +v` 逐字节相同 | 编码门禁 2l 段 | 通过 |
| V-MC2 | `vsetivli` 的 token 与数字 vtype 产生相同编码 | `v-extension.s` 的 `0xd0` 用例 | 通过 |
| V-MC3 | 反汇编能打印 `v0`–`v31` 和 `e32, m1, ta, ma` | `roundtrip v` + `DIS` | 通过 |
| V-MC4 | 缺 `+v` 时报 `'V' (Vector Extension)` | `v-extension.s` 的 MISSING | 通过 |
| V-MC5 | 未知 SEW、越界 vtype、越界 `vadd.vi` 有清晰诊断 | BADTYPE/RANGE/RANGE10/RANGEI | 通过 |
| V-MC6 | `-mcpu=generic-rv32-v` 等价于 `-mattr=+v` | CPU 前缀检查 | 通过 |
| V-MC7 | `rv32imafdv` 可编译，`rv32imafdc`/`rv32gc` 不回归 | clang 矩阵 432 对象 | 通过 |
| V-MC8 | 代码生成不回归：8/8 门禁、lit 全绿 | 本章实测输出 | 通过 |

## 常见坑

1. **`vtypei` 的隐含位。** `vsetivli` 只编码 10 位，`vtypei[10]` 由
   `Inst{31:30}=11` 隐含；不要把 11 位字段硬塞进 10 位位置。
2. **`vadd.vi` 是有符号 5 位。** 16 会被拒绝；`-16..15` 才是合法范围。
   用 `simm5` 和 `decodeSImmOperand<5>`，不要复用 `uimm5`。
3. **`vmul` 的 funct3 不是 000/100。** 它在 OPMVV/OPMVX：`.vv=010`、
   `.vx=110`。照抄 `vadd` 会得到完全不同的指令。
4. **V 寄存器不能放进 `RegAltNameIndices` 块。** 该块要求每个寄存器都有
   alt name；`v0` 没有 ABI 别名，放进去会报
   `Register definition missing alt name for 'ABIRegAltName'`。
5. **自定义操作数解析必须在 `parseOperand` 最前。** `parseVType` 依赖
   `MatchOperandParserImpl` 先拿到 vtype 位置；否则 `e32` 会先被当作符号。
6. **逗号由 `parseVType` 自己消费。** 外层 `parseInstruction` 只在操作数
   之间吃逗号；vtype 内部的三个逗号必须在自定义解析器里吃掉，否则会把
   `m1` 当成下一条指令的操作数。
7. **clang 的 `v` 会展开。** 只接受 `v` 不够；`zve*`/`zvl*` 也要在前端
   白名单里，但最终 target-features 只保留 `+v`。
8. **本章不生成向量代码。** `-mattr=+v` 的 `llc` 输出仍是标量；不要用
   `llc` 找 `vadd.vv`，那是第 48 章的目标。

## 练习

1. 加上 `vle8.v`/`vse8.v`：width 字段从 `110` 改成 `000`，观察编码。
2. 给 `vadd.vv` 加上 mask 形式（`vm=0`，`v0` 作为 mask），需要新的
   `vmv.mm` 或 `vmerge` 指令配合；先只做编码，不做代码生成。
3. 把 `vsetvli` 的 vtype 打印扩展成支持 `mf8/mf4/mf2/m2/m4/m8`，并写一个
   反汇编往返测试。
4. 让 `parseVType` 接受 `e32, m1, tu, mu`，确认打印器输出 `tu, mu` 且
   编码的 bit 6/7 为 0。
5. 用 `llvm-mc -triple=riscv32 -mattr=+v` 生成一份随机 vtype 表，和 RVMin
   的编码逐项对比，找出保留编码的差异。
