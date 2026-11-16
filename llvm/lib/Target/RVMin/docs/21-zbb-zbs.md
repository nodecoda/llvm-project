# 第 21 章：Zbb / Zbs 位操作扩展

## 目标

第 10 章把 `clz`/`cpop`/`bswap` 展开成十几条算术指令，因为 RV32I 没有位操作。
本章加上两个可选扩展：

- **Zbb**（Basic bit manipulation）：`clz ctz cpop sext.b sext.h rev8 zext.h
  rol ror rori andn orn xnor min minu max maxu`；
- **Zbs**（Single-bit instructions）：`bset bclr binv bext` 及其立即数形式。

完成本章后：

1. `-mattr=+zbb` 下 `clz/cpop/rev8` 各一条指令，`smin/umax` 变成 `min/maxu`；
2. `-mattr=+zbs` 下 `x | (1<<n)`、`x & ~(1<<n)` 等惯用法变成 `bset/bclr`；
3. 不开扩展时输出与第 20 章**逐字节一致**（第 10 章的展开原样保留）；
4. 27 条新指令的编码与参考 RISC-V 汇编器逐字节相同，缺特性时诊断正确；
5. 理解“扩展是可选子目标特性”这句话在 TableGen、legalizer、汇编器三处分别意味着什么。

## 背景：为什么位操作要单独成扩展

RV32I 的移位/逻辑指令只能表达“一位一位算”的算法。以 `clz`（数前导零）为例，
第 10 章的展开是一个带分支的循环；`cpop` 是五次“掩码-移位-加”；`bswap` 是四组
移位与掩码。Zbb 把这些变成单条指令，代码尺寸和延迟都降一个数量级。

但 RV32I 生态里大量芯片没有 Zbb，所以它必须是**可选**的：默认子目标不能改变。
本章的结构与第 13 章（M 扩展）完全同构：

| 层 | M 扩展（第 13 章） | Zbb/Zbs（本章） |
| --- | --- | --- |
| 特性定义 | `FeatureStdExtM` | `FeatureStdExtZbb`、`FeatureStdExtZbs` |
| 谓词 | `HasStdExtM` | `HasStdExtZbb`、`HasStdExtZbs` |
| Subtarget 字段 | `HasStdExtM` | `HasStdExtZbb`、`HasStdExtZbs` |
| 指令/编码 | 8 条 R 型 | 17 条 Zbb + 8 条 Zbs |
| legalizer | `MUL/DIV/...` 保持 Legal | 位操作从 `Expand` 改 `Legal` |
| 默认行为 | libcall | 第 10 章的算术展开 |

**QEMU 的坑（先说结论）**：本机的 `qemu-riscv32-static` 不支持 Zbb——执行 `cpop`
会以 `Illegal instruction`（退出码 132）终止，`-cpu rv32,zbb=true` 也被拒绝。
所以本章的运行门禁不是 QEMU，而是“与参考汇编器编码对照 + 默认/扩展两套输出对照”。

## 代码地图

| 位置 | 职责 |
| --- | --- |
| `RVMin.td:34-44` | 两个 `SubtargetFeature` 与两个 `Predicate` |
| `RVMin.td:57-58` | `-mcpu=generic-rv32-zbb/zbs` 处理器定义 |
| `RVMinSubtarget.h:40-42,56-58` | `HasStdExtZbb/Zbs` 字段与 `hasStdExtZbb()/hasStdExtZbs()` |
| `RVMinISelLowering.cpp:70-76` | `SIGN_EXTEND_INREG`：i8/i16 在 Zbb 下改 `Legal` |
| `RVMinISelLowering.cpp:121-137` | `ROTL/ROTR/CTPOP/CTLZ/CTTZ/BSWAP` 与 `SMIN/SMAX/UMIN/UMAX` 的动作 |
| `RVMinInstrInfo.td`（Zbb/Zbs 段） | 两个一元格式类、25 条指令、选择 pattern |
| `scripts/rvmin-zbb-encoding-corpus.s` | 19 条 Zbb 编码语料 |
| `scripts/rvmin-zbs-encoding-corpus.s` | 8 条 Zbs 编码语料 |
| `scripts/check-rvmin-encodings.sh:103-140` | 与 `-triple=riscv32` 的编码对照 + 缺特性诊断 |
| `llvm/test/CodeGen/RVMin/zbb-zbs.ll` | 17 个函数 × {`+zbb`,`+zbs`,默认} 的 lit |
| `llvm/test/MC/RVMin/zbb-zbs.s` | 25 条编码固定值 + 两条诊断 |

## 关键实现

### 1. 特性三件套：TableGen 是唯一的开关

```tablegen
def FeatureStdExtZbb : SubtargetFeature<"zbb", "HasStdExtZbb", "true",
                                        "Basic bit manipulation">;
def HasStdExtZbb : Predicate<"Subtarget->hasStdExtZbb()">,
                   AssemblerPredicate<(all_of FeatureStdExtZbb),
                                      "'Zbb' (Basic Bit-Manipulation)">;
```

- `SubtargetFeature` 让 `-mattr=+zbb` 写入 `HasStdExtZbb` 字段；
- `Predicate` 让指令选择器只在特性打开时使用这些指令；
- `AssemblerPredicate` 让汇编器在缺特性时报
  `instruction requires the following: 'Zbb' (Basic Bit-Manipulation)`。

三者共用同一个名字，任何一处写错都会在 lit 里立刻暴露。

### 2. 编码：两类新格式

一元 Zbb 指令都是 I 型，但 `imm[11:0]` 是固定的：

```tablegen
class RVMinZbUnary<bits<12> imm12, bits<3> funct3, string mnemonic>
    : RVMinInst<(outs GPR:$rd), (ins GPR:$rs1), mnemonic # " $rd, $rs1", []> {
  bits<5> rd;
  bits<5> rs1;
  let Inst{6-0}   = OPC_IMM.Value;
  let Inst{11-7}  = rd;
  let Inst{14-12} = funct3;
  let Inst{19-15} = rs1;
  let Inst{31-20} = imm12;
}
```

`clz/ctz/cpop/sext.b/sext.h` 用 funct7=`0b0110000`、低 5 位 0/1/2/4/5；
`rev8` 是 funct3=101、imm=0x698。`zext.h` 是 R 型但 `rs2` 固定为 0
（`pack rd, rs1, x0`），所以另立一个一元 R 型类。

`rol` 没有立即数编码：`roli n` 等于 `rori 32-n`，用 `ImmSub32` 这个
`SDNodeXForm` 在匹配时算出来。

### 3. legalizer：`Expand` 还是 `Legal`

```cpp
const TargetLowering::LegalizeAction BitAction =
    STI.hasStdExtZbb() ? Legal : Expand;
setOperationAction(ISD::ROTL, VT, BitAction);
...
if (STI.hasStdExtZbb())
  setOperationAction({ISD::SMIN, ISD::SMAX, ISD::UMIN, ISD::UMAX}, VT, Legal);
```

`Legal` 不是“已经有指令”的同义词：它告诉 legalizer **不要展开**，把节点留给
指令选择。因此每加一个 `Legal`，就必须同时有一条能匹配它的 pattern，否则
`llc` 会在指令选择阶段报 “Cannot select”。

`SIGN_EXTEND_INREG` 有个反直觉点：注册动作时用的 VT 是**被扩展的窄类型**
（`i8`/`i16`），不是结果类型 `i32`。所以代码是：

```cpp
setOperationAction(ISD::SIGN_EXTEND_INREG, MVT::i1, Expand);
for (MVT VT : {MVT::i8, MVT::i16})
  setOperationAction(ISD::SIGN_EXTEND_INREG, VT,
                     STI.hasStdExtZbb() ? Legal : Expand);
```

只给 `MVT::i32` 注册（第 9 章的写法）对 `sext.b` 完全无效。

### 4. 三个“规范形”陷阱

**`zext.h`**：`(x & 0xFFFF)` 的掩码放不进 12 位立即数，默认会
`lui+addi` 造一个寄存器再 `and`。Zbb 的 pattern 必须直接匹配常量：

```tablegen
def : Pat<(and GPR:$rs1, 0xFFFF), (ZEXT_H GPR:$rs1)>;
```

**`bset/bclr` 与 ROTL 的相互作用**：Zbb 让 `ROTL` 变成 Legal 之后，DAG combiner
会把 `1 << n` 规范化为 `rotl 1, n`，把 `~(1 << n)` 规范化为 `rotl -2, n`。
如果 pattern 只写 `shl 1, n`，`+zbb,+zbs` 同时打开时反而匹配不到。两种拼写都要：

```tablegen
def : Pat<(shl 1, GPR:$rs2), (BSET X0, GPR:$rs2)>;
def : Pat<(rotl 1, GPR:$rs2), (BSET X0, GPR:$rs2)>;
def : Pat<(and (rotl -2, GPR:$rs2), GPR:$rs1), (BCLR GPR:$rs1, GPR:$rs2)>;
```

**`andn/orn/xnor`**：IR 里是 `and x, (xor y, -1)`，DAG 里 `xor -1` 会变成 `not`。
pattern 写 `(and (not GPR:$rs1), GPR:$rs2)`；AND/OR/XOR 可交换，
TableGen 会自动尝试另一种操作数顺序。

## 构建与验证

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop
ninja -C "$B" llc llvm-mc

# 1. lit：17 个函数在 +zbb / +zbs / 默认三种配置下的形状
"$B/bin/llvm-lit" -v "$W/llvm/test/CodeGen/RVMin/zbb-zbs.ll" \
                      "$W/llvm/test/MC/RVMin/zbb-zbs.s"

# 2. 编码 + 缺特性诊断（已并入第 3 项全门禁）
"$W/llvm/lib/Target/RVMin/scripts/check-rvmin-encodings.sh" "$B/bin"

# 3. 全部 6 项门禁（lit 现在是 22 个测试、120 次 -verify-machineinstrs）
"$W/llvm/lib/Target/RVMin/scripts/rvmin-verify-all.sh" "$B/bin"
```

手工观察：

```bash
"$B/bin/llc" -march=rvmin -mtriple=riscv32-unknown-elf -mattr=+zbb -O2 \
  "$W/llvm/test/CodeGen/RVMin/zbb-zbs.ll" -o - | grep -E '^\s+(clz|cpop|rev8)'
```

## 预期输出

`+zbb`：

```asm
clz:
	clz a0, a1
cpop:
	cpop a0, a0
rev8:
	rev8 a0, a0
smin:
	min a0, a0, a1
umax:
	maxu a0, a0, a1
zext_h:
	zext.h a0, a0
sext_b:
	sext.b a0, a0
```

`+zbs`：

```asm
bset:
	bset a0, a0, a1
bclr:
	bclr a0, a0, a1
binv:
	binv a0, a0, a1
bext:
	bext a0, a0, a1
```

默认（无扩展）一条都没有：

```text
PASS 6/6: 120 llc runs with -verify-machineinstrs
```

编码门禁：

```text
PASS: 85 RV32I + 12 M + 19 Zbb + 8 Zbs instructions, .text identical
```

## 常见坑

### 坑 1：只加指令、忘了 `Predicates`

没有 `let Predicates = [HasStdExtZbb]` 的指令会进入默认指令选择，默认输出
立刻改变，第 10 章的展开测试全部失败。汇编器也会在没开特性时接受这些指令。

### 坑 2：`SIGN_EXTEND_INREG` 注册到了 `i32`

legalizer 查询的是窄类型。第 9 章的循环里如果直接改成
`setOperationAction(ISD::SIGN_EXTEND_INREG, MVT::i32, Legal)`，`sext.b` 永远不会
出现——节点在 legalize 阶段就被展开成移位对了。

### 坑 3：`ROTL` Legal 之后 `1 << n` 变了形状

上面已经说明：`shl 1, n` 会变成 `rotl 1, n`。这不是 bug，而是
“让一个操作 legal 会改变 DAG 规范形”的典型例子。写 pattern 前先用
`-print-after=combine` 看一眼真实形状。

### 坑 4：给 `Legal` 加了动作却没有 pattern

`Legal` 意味着“保持节点不变，交给指令选择”。少写一条 pattern 的直接后果是
`llc: error: Cannot select: ...`，而不是悄悄退化。lit 的三种配置正是为此。

### 坑 5：以为 QEMU 能跑 Zbb

本机 QEMU 的 `rv32` CPU 没有 Zbb/Zbs 属性，执行 `cpop` 直接
`Illegal instruction`。本章没有 QEMU 门禁；如果以后换了支持 Zbb 的模拟器，
可以把 `scripts/rt/bitmanip.c` 加进 `rvmin-run.sh`。

### 坑 6：`rori` 打印成 `rori`，而不是 `ror`

`ror` 是寄存器形式（R 型），`rori` 是立即数形式（I 型）。二者 funct7 相同、
funct3 也相同，只有 opcode 不同；TableGen 的两个类已经区分，写 pattern 时别混。

## 练习

1. 给 `ctz` 加一条 `ctz(x) = 32 - clz(~x & (x-1))` 的测试，确认 `+zbb` 下仍是一条。
2. 实现 `orc.b`（Zbb 的字节或）与 `rev8` 的对比，观察 `bswap` 的 pattern 是否仍然命中。
3. 给 `bseti/bclri/binvi/bexti` 加常量匹配：`x | (1 << 20)` 在 `+zbs` 下应变成
   `bseti a0, a0, 20`（注意 20 超出 `simm12` 的位模式，但 Zbs 的 5 位立即数够用）。
4. 用 `-print-after=combine` 对比 `-mattr=+zbb` 与默认下 `1 << n` 的 DAG。
5. 把 `generic-rv32-zbb` 与 `generic-rv32-m` 组合成 `generic-rv32-m-zbb`，
   验证 `-mcpu` 能同时打开两个特性。

## 下一章

第 22 章实现 A 扩展：`lr.w/sc.w`、`amo*`、`fence`，并让原子操作在
`-mattr=+a` 下不再展开成 libcall。
