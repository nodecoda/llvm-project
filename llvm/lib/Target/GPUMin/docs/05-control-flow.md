# 第 05 章：控制流、标签与 PC 相对 fixup

## 目标

第 04 章的函数是一条直线。本章加分支，并第一次引入"依赖布局"的编码：

1. B 格式：`bra label` 与 `bra pN, label`，16 位**字**偏移；
2. bit 25 区分无条件与条件分支——`p0` 不等于"总是"；
3. 新 fixup `fixup_gpumin_16_pcrel`，把标签解析成 `(target - PC) / 4`；
4. `br i1` / `icmp+br` 分别走 `BRCOND` / `BR_CC` lowering，条件必须是
   谓词寄存器；
5. if/else 与带 phi 的循环端到端通过，`.o` 与"汇编 `.s` 再汇编"一致；
6. 越界位移和非标签目标都明确报错。

退出条件（可执行）：

```sh
llvm/lib/Target/GPUMin/scripts/check-ch05.sh <llvm-bin-dir>
```

## 背景

### 为什么偏移按"字"计

GM32 每条指令 4 字节。如果偏移按字节存，16 位只能覆盖 ±32 KiB，而且
永远低两位为 0（浪费 2 bit）。按字存：范围 ±32Ki 条指令（±128 KiB），
并且对齐是天然的——任何 `target - PC` 都是 4 的倍数，除以 4 之后不会
出现"半个字"的地址。

代价是**所有**读这个字段的地方都要知道单位是字：编码器、applyFixup、
反汇编器、模拟器。本章把这四个地方都改了一遍，文档里也统一用"字"。

### 为什么需要 fixup

汇编时 `bra .L1` 只知道自己要跳到一个还没确定地址的标签。真实地址要等
所有指令都排好序才知道，所以编码阶段先写 0 并留下一条 fixup，布局完成
后由 `MCAsmBackend::applyFixup` 回填。

MC 已经替我们算好了一部分：fixup 创建时 `IsPCRel = true`，assembler 在
求值时自动把 `Value` 变成 `target - instruction_address`。所以 backend
里看到的 `Value` 已经是**字节位移**，只需要除以 4。

### 为什么数字目标被拒绝

`bra 2` 看起来应该表示"向前跳 2 条指令"，但编码器在任何给定时刻都不
知道这条指令自己的地址——它只知道当前字节流写到哪了。没有地址就算不出
相对位移，fixup 也没法用一个纯常量解析（assembler 只在有符号时才认为
PC 相对 fixup 已解析）。本章的选择是：**汇编器只接受标签**，数字偏移
只出现在反汇编输出和 Python 编码器里。

拒绝的信息要早、要准：

```text
$ printf 'bra 2\n' | llvm-mc -triple=gpumin
<stdin>:1:5: error: invalid operand for instruction
bra 2
    ^
```

如果不拒绝，错误会晚到 relocation 阶段，变成
`GPUMin: unsupported relocation`——完全看不出是"数字目标"的问题。

## 设计

### 位域

```text
31    26 25 24  22 21      16 15                 0
 opcode   1  pred     0            off16
```

- 无条件：`hasPred = 0`，`pred = 0`；
- 条件：`hasPred = 1`，`pred = pN`。

为什么不用 `pred = 0` 表示"无条件"？因为 `p0` 是合法谓词，"总是执行"
和"当 p0 为真"是两种不同语义。第 14 章做分歧时这个区别会变成真问题：
warp 级分支和 lane 级谓词是两条不同的路径。

### 指令

| 汇编 | 编码 | 说明 |
| --- | --- | --- |
| `bra .L1` | bit25=0 | 无条件 |
| `bra p1, .L1` | bit25=1, pred=1 | 谓词为真时跳转 |

两条指令共用 opcode `0x10`。`BRA` 是 `isBarrier`（流到此为止），
`BRA_P` 不是（条件为假时继续执行下一条）。

### fixup 语义

```text
assemble:   off16 = 0，记录 fixup(target=.L1, kind=16_pcrel, IsPCRel=true)
layout:     Value = address(.L1) - address(bra)
applyFixup: Off = Value / 4
            检查对齐（Value & 3）
            检查 isInt<16>(Off)
            写 Data[0..1]（Inst[15:0]）
```

fixup 的 offset 必须是 0：立即数在 `Inst[15:0]`，也就是指令的最低两个
字节，正好是 `Data` 指向的位置。如果哪天把偏移挪到 `Inst[31:16]`，
`MCFixupKindInfo` 的 `TargetOffset` 也要跟着改成 16。

## 实现

### 1. `.td`

```tablegen
def brtarget16 : Operand<OtherVT> {
  let PrintMethod = "printOperand";
  let ParserMatchClass = BranchTargetAsmOperand;
  let EncoderMethod = "getBranchTargetOpValue";
  let DecoderMethod = "DecodeBrTarget16";
}

def BRA   : GPUMinBra<OPC_BRA.Value, (ins brtarget16:$off), "bra $off", []> {
  let isBranch = 1; let isTerminator = 1; let isBarrier = 1;
}
def BRA_P : GPUMinBraP<OPC_BRA.Value, "bra $pd, $off"> {
  let isBranch = 1; let isTerminator = 1;
}
```

操作数类型是 `OtherVT` 而不是 `i32`：分支目标永远不会变成寄存器里的值，
用 `OtherVT` 让 TableGen 在类型检查时拒绝"把分支目标当数据用"。

### 2. 模式与目标节点

```tablegen
def SDT_GPUMinBrCond : SDTypeProfile<0, 2, [SDTCisVT<0, i1>]>;
def GPUMinBrCond : SDNode<"GPUMinISD::BRCOND", SDT_GPUMinBrCond,
                          [SDNPHasChain]>;

def : Pat<(br bb:$dst), (BRA brtarget16:$dst)>;
def : Pat<(GPUMinBrCond i1:$pd, bb:$dst), (BRA_P $pd, brtarget16:$dst)>;
```

`SDTNone` 在这里不能用：它声明"零个操作数"，而 BRCOND 有两个
（谓词、目标）。写成 `SDTNone` 会在 tblgen 阶段报
`GPUMinBrCond node requires exactly 0 operands!`。

### 3. lowering

```cpp
case ISD::BRCOND:
  return DAG.getNode(GPUMinISD::BRCOND, DL, MVT::Other,
                     Op.getOperand(0), Op.getOperand(1), Op.getOperand(2));

case ISD::BR_CC: {
  SDValue Cmp = DAG.getSetCC(DL, MVT::i1, Op.getOperand(2),
                             Op.getOperand(3), CC);
  return DAG.getNode(GPUMinISD::BRCOND, DL, MVT::Other, Chain, Cmp,
                     Op.getOperand(4));
}
```

动作注册有一个容易写错的细节：

```cpp
setOperationAction(ISD::BRCOND, MVT::Other, Custom);  // 对
setOperationAction(ISD::BR_CC, MVT::i32,   Custom);  // 不是 MVT::Other
```

`BR_CC` 的合法化按**比较操作数**的类型查表，写成 `MVT::Other` 时
legalizer 根本不会调用我们的 lowering，节点直接落到 ISel 报
`Cannot select: br_cc`。

### 4. 编码器

```cpp
uint64_t getBranchTargetOpValue(const MCInst &MI, unsigned OpNo, ...) {
  const MCOperand &MO = MI.getOperand(OpNo);
  assert(MO.isExpr() && "branch target must be a symbol");
  Fixups.push_back(MCFixup::create(0, MO.getExpr(),
                                   (MCFixupKind)fixup_gpumin_16_pcrel,
                                   /*IsPCRel=*/true));
  return 0;
}
```

`EncoderMethod` 的第二个参数是**操作数下标**（`unsigned OpNo`），不是
`MCOperand`。写成 `const MCOperand &` 会在生成的 `.inc` 里报
`cannot convert 'int' to 'const MCOperand&'`。

### 5. backend

```cpp
int64_t Off = static_cast<int64_t>(Value);   // 已经是字节位移
if (Off & 3) reportError(...);
Off /= 4;
if (!isInt<16>(Off)) reportError("GPUMin: branch target out of range");
Data[0] = Off & 0xff;
Data[1] = (Off >> 8) & 0xff;
```

### 6. 反汇编器

```cpp
static DecodeStatus DecodeBrTarget16(MCInst &Inst, uint64_t Imm, ...) {
  const int64_t Words = SignExtend64<16>(Imm);
  if (!Decoder->tryAddingSymbolicOperand(Inst, Words * 4, Address,
                                         /*IsBranch=*/true, 0, 4, 4))
    Inst.addOperand(MCOperand::createImm(Words));
  return MCDisassembler::Success;
}
```

`tryAddingSymbolicOperand` 要的是**字节**位移（它自己加指令地址），所以
这里乘 4；操作数本身仍然打印成字，和汇编器的输入单位一致（虽然汇编器
只接受标签）。

## 代码走读

### 一条 `bra .L1`

```text
llvm-mc
 └─ AsmParser: 操作数 kind = Immediate(expr = MCSymbolRefExpr .L1)
     └─ BranchTargetAsmOperand::isBranchTarget -> 非常量，接受
 └─ MCCodeEmitter::getBranchTargetOpValue
     └─ fixup(off=0, kind=16_pcrel, IsPCRel=true), 返回 0
 └─ MCAssembler 布局完成
     └─ Value = addr(.L1) - addr(bra)
     └─ applyFixup: Off = Value/4 -> Data[0..1]
```

### 循环的产物

```llvm
loop:
  %i   = phi i32 [0, %entry], [%i1, %body]
  %acc = phi i32 [0, %entry], [%acc1, %body]
  %c   = icmp slt i32 %i, %n
  br i1 %c, label %body, label %exit
```

```asm
sum:
	movi r2, 0
	mov  r1, r2
.LBB0_1:
	setp.ge p0, r2, r4
	bra  p0, .LBB0_3
	bra  .LBB0_2
.LBB0_2:
	movi r3, 1
	add  r3, r2, r3
	add  r1, r1, r2
	mov  r2, r3
	bra  .LBB0_1
.LBB0_3:
	mov  r4, r1
	exit
```

注意 `setp.ge` 是 `icmp slt` 取反后的结果：`br i1 %c, %body, %exit` 在
机器层是"条件不成立时跳到 exit"，所以比较关系被反过来。`phi` 边上的
拷贝变成了 `mov`，回边 `bra .LBB0_1` 的位移是负数。

## 验证

```sh
$ llvm/lib/Target/GPUMin/scripts/check-ch05.sh <build>/bin
PASS 1/4: forward and backward labels resolve to word displacements
PASS 2/4: unconditional and predicated branches encode differently
PASS 3/4: basic blocks lower to branches and survive the round trip
PASS 4/4: out-of-range and non-label targets are rejected; bytes agree
OK: chapter 05 gate passed
```

第 4 项把"标签对象里的 `.text` 字节"和"Python 编码器按解析后位移算出的
字节"直接比较：`bra .L1` 在偏移 0 处、目标在偏移 8 处，独立编码器写的
`bra 2` 必须给出一样的三个字。

## 常见坑

1. **`BR_CC` 的 action 键不是 `MVT::Other`**。写成 `Other` 时 legalizer
   跳过 custom lowering，节点一路走到 ISel 才报 `Cannot select: br_cc`。
2. **`SDTNone` 只能描述零操作数节点**。`BRCOND` 有两个操作数，必须定义
   `SDTypeProfile<0, 2, ...>`。
3. **`EncoderMethod` 的第二个参数是下标**。写 `const MCOperand &` 报
   `cannot convert 'int' to 'const MCOperand&'`，而且错误在生成的 `.inc`
   里，不在自己的代码里。
4. **PCRel 值已经被减过指令地址**。在 `applyFixup` 里再减一次会得到
   "目标 - 2 * PC"，通常表现为跳转偏移恰好差一条指令的位置。
5. **数字目标只能晚失败**。允许 `bra 2` 的话，它在编码期拿不到地址，
   最终在 relocation 阶段报 `unsupported relocation`；在 parser 里用
   `isBranchTarget` 提前拒绝，错误信息才对得上用户输入。
6. **`isBarrier` 只给无条件分支**。给 `BRA_P` 加 `isBarrier` 会让
   block placement 认为条件分支之后没有 fallthrough，生成的控制流图
   和 IR 不一致。
7. **偏移单位是字**。编码器、fixup、反汇编器、模拟器四处必须一致；
   少乘/多乘 4 的 bug 只有在"跳转落到指令中间"或者"跑飞"时才暴露。

## 练习

1. 给 `BRA` 加 `AnalyzeBranch`/`insertBranch`/`removeBranch` 的实现，然后
   用 `-print-after=block-placement` 观察分支折叠和块合并是否变多。
2. 用 `llvm-objdump -d --symbolize-operands` 看反汇编能不能打印标签。
   如果不能，检查 `tryAddingSymbolicOperand` 的参数里哪一项需要调整。
3. 把 `off16` 改成**字节**偏移（16 位），同步改四处，跑一遍 gate。
   解释为什么"按字"是对 SIMT 更友好的选择（提示：一个 warp 32 lane，
   对齐错误会让整个 warp 的行为不可预测）。
4. 写一个三层嵌套循环，观察 `.LBB` 编号与回边数量。再打开
   `-passes=machine-block-placement` 的调试输出，看 block placement 怎么
   选择 fallthrough。
5. 实现 `bra` 的 long 形式（32 位偏移），说明它在 M 格式/B 格式之间的
   取舍，以及为什么第 05 章不做。

## 下一章

第 06 章把 `ld/st` 从 I 格式（16 位位移、无空间/宽度信息）改成 M 格式：
`[10:8] space / [7:6] size / [5] sign`，同时接上 `addrspace(1)` 全局内存
和第一批重定位。分支的 PC 相对 fixup 会是那时"跨节引用"的参照物。
