# 第 04 章：整数 ALU、谓词比较与选择

## 目标

第 03 章的 `.ll` 里只能出现常量和参数搬移。本章让算术表达式真正生成
指令，并第一次用上 `p0-p7`：

1. 八条整数 ALU（`add/sub/and/or/xor/shl/shr/sar`）各有一条 SelectionDAG
   模式，`funct` 与 DAG 节点一一对应；
2. `icmp` 的十二种关系（6 个有符号 + 6 个无符号）都有 `setp.*`；
3. `select` 走 `SELP`，比较结果**不**物化成 0/1 的 GPR；
4. `i1` 成为合法类型，落在 `PRED` 寄存器类；
5. 独立编码器覆盖 ALU/SETP/SELP，与 `llvm-mc` 逐字节一致。

退出条件（可执行）：

```sh
llvm/lib/Target/GPUMin/scripts/check-ch04.sh <llvm-bin-dir>
```

## 背景

### 为什么比较结果进谓词寄存器

GPU 上"每 lane 一个条件"是最常见的值：一个 warp 32 个 lane，每次比较
产生 32 个 bit。把它物化成 32 个 32 位 GPR 是 32 倍的浪费，而且分支
（第 05 章）本来就要读谓词。

所以 GM32 把比较结果放进 `p0-p7`，一个谓词寄存器每 lane 只存 1 位。
对 LLVM 来说，这意味着 `i1` 必须是一个**合法类型**，并且有对应的
`RegisterClass`：

```cpp
addRegisterClass(MVT::i1, &GPUMin::PREDRegClass);
setBooleanContents(ZeroOrOneBooleanContent);
```

`setBooleanContents` 不是可有可无的装饰：它告诉 legalizer "i1 的真值
在需要转成整数时按 0/1 解释"。少了它，某些合并会把 `i1` 当成 0/-1
处理，生成的 `SELP` 两个源会反。

### 为什么 `icmp + select` 会变成 `select_cc`

SelectionDAGBuilder 阶段，`icmp` 和 `select` 是两个节点；类型合法化之后，
`select` 的条件（i1）在很多目标上会被折叠成 `select_cc`，把比较和选择
合成一个节点。所以本章要处理的是 `ISD::SELECT_CC`，而不是 `ISD::SELECT`：

```cpp
case ISD::SELECT_CC: {
  SDValue Cmp = DAG.getSetCC(DL, MVT::i1, LHS, RHS, CC);
  return DAG.getSelect(DL, Op.getValueType(), Cmp, TrueV, FalseV);
}
```

拆成 `setcc` + `select` 之后，两条模式各生成一条指令：`SETP` 写谓词，
`SELP` 消费谓词。这正是硬件的样子。

### 本章的常量边界

`movi` 只有 16 位。本章只支持 `[-32768, 32767]` 的常量；更大的值会在
`Select()` 里落空，报 `Cannot select: ... Constant<...>`。

第 08 章把边界推进到 `[0, 65535]`：`movi` 会符号扩展，所以无符号的 16
位常量再补一对 `shl/shr` 清零高位。完整的 32 位常量需要"高位 `movi` +
`shl` + 低位 `movi` + `shl/shr` + `or`"，那个序列后来补上了（见第 08 章
的常量表和 `demos/02-alu-and-constants.ll`）。

## 设计

### 指令编码回顾

`ALU`（opcode `0x02`）的 `funct[10:0]` 低四位选操作：

| funct | 指令 | DAG 节点 |
| --- | --- | --- |
| 0 | `add` | `ISD::ADD` |
| 1 | `sub` | `ISD::SUB` |
| 2 | `and` | `ISD::AND` |
| 3 | `or` | `ISD::OR` |
| 4 | `xor` | `ISD::XOR` |
| 5 | `shl` | `ISD::SHL` |
| 6 | `shr` | `ISD::SRL` |
| 7 | `sar` | `ISD::SRA` |

注意三个移位是三个**不同**的 DAG 节点：`shl`、逻辑右移 `srl`、算术右移
`sra`。它们共用一条指令格式，只有 funct 不同——这是最容易被
"复制粘贴改错一个数字"坑到的地方，所以门禁逐条检查。

`SETP`（opcode `0x03`）的 `funct = { unsigned, relation[2:0] }`：

| funct | 关系 | funct | 关系 |
| --- | --- | --- | --- |
| 0 | `eq` | 8 | `ueq` |
| 1 | `ne` | 9 | `une` |
| 2 | `lt` | 10 | `ult` |
| 3 | `le` | 11 | `ule` |
| 4 | `gt` | 12 | `ugt` |
| 5 | `ge` | 13 | `uge` |

bit 3 就是"无符号"开关。`setp.lt` 和 `setp.ult` 的差别只有这一位，
门禁用 `icmp slt` / `icmp ugt` 两个函数来钉住它。

### `SELP` 的位域

`SELP`（opcode `0x04`）需要三个寄存器和一个谓词：

```text
31    26 25   21 20   16 15   11 10  8 7        0
 opcode     rd      rs1     rs2     pd     0
```

`pd` 只有 3 位（`p0-p7`），塞在原本 R2 的 funct 区高三位，`[7:0]` 留 0。
这样 `rd/rs1/rs2` 仍在老位置，编码器、打印器和解码器都不需要新逻辑；
代价是 `funct` 区不再"整块可用"，将来如果 `SELP` 需要条件修饰，得换
布局。

### i1 到 i32 的扩展

`icmp` 的结果如果直接当整数用（`select i1 %c, i32 1, i32 0` 经常被
InstCombine 改写成 `zext i1 %c`），谓词里的 1 bit 要变成 32 位值。
本章在 `Select()` 里显式生成三条机器指令：

```text
zext i1 %p   ->  movi r1, 1
                 movi r2, 0
                 selp rd, %p, r1, r2

sext i1 %p   ->  movi r1, -1
                 movi r2, 0
                 selp rd, %p, r1, r2
```

这是"没有专用指令时，用谓词选择造一个"的最小例子。

## 实现

### 1. 模式（`GPUMinInstrInfo.td`）

```tablegen
def : Pat<(add  GPR:$rs1, GPR:$rs2), (ADD $rs1, $rs2)>;
...
def : Pat<(srl  GPR:$rs1, GPR:$rs2), (SHR $rs1, $rs2)>;
def : Pat<(sra  GPR:$rs1, GPR:$rs2), (SAR $rs1, $rs2)>;

def : Pat<(setcc GPR:$rs1, GPR:$rs2, SETLT), (SETP_LT $rs1, $rs2)>;
def : Pat<(select i1:$pd, GPR:$rs1, GPR:$rs2), (SELP $pd, $rs1, $rs2)>;
```

`(select i1:$pd, ...)` 能成立的前提就是前面那个
`addRegisterClass(MVT::i1, ...)`；没有它，TableGen 会拒绝把 `i1` 绑定到
`PRED` 的寄存器操作数。

### 2. 合法化设置

```cpp
addRegisterClass(MVT::i1, &GPUMin::PREDRegClass);
setBooleanContents(ZeroOrOneBooleanContent);
setOperationAction(ISD::SELECT, MVT::i32, Legal);
setOperationAction(ISD::SELECT_CC, MVT::i32, Custom);
setOperationAction(ISD::SETCC, MVT::i32, Legal);
```

`SELECT` 是 `Legal` 因为模式已经写好了；`SELECT_CC` 是 `Custom` 因为它
要先拆成 `setcc + select`。`SETCC` 也是 `Legal`：`i1` 合法、模式齐备，
legalizer 不需要再动它。

### 3. `LowerOperation`

`LowerOperation` 从第 00 章的 `llvm_unreachable` 变成真正的分派：

```cpp
switch (Op.getOpcode()) {
default:
  llvm_unreachable("GPUMin: unhandled custom lowering");
case ISD::SELECT_CC: { ... }
}
```

这个 switch 会随着章节增长（第 05 章加 `BR_CC`，第 06 章加
`LOAD/STORE` 的地址折叠，第 08 章加小整数类型）。

### 4. `Select()` 里的两种扩展

```cpp
if (N->getOpcode() == ISD::ZERO_EXTEND &&
    N->getOperand(0).getValueType() == MVT::i1) {
  SDValue One  = SDValue(CurDAG->getMachineNode(MOVI, ...1...), 0);
  SDValue Zero = SDValue(CurDAG->getMachineNode(MOVI, ...0...), 0);
  ReplaceNode(N, CurDAG->getMachineNode(SELP, DL, MVT::i32,
                                        N->getOperand(0), One, Zero));
}
```

`getMachineNode` 返回 `MachineSDNode *`，要包成 `SDValue(node, 0)` 才能
当操作数用——本章实现时这里是一串编译错误，不是运行期 bug。

## 代码走读

### `icmp slt` + `select`

```llvm
%c = icmp slt i32 %a, %b
%d = select i1 %c, i32 %a, i32 %b
ret i32 %d
```

```text
SelectionDAGBuilder   select_cc(%a, %b, %a, %b, setlt)
类型合法化             保持 i32，i1 由 SETCC 产生
LowerOperation        setcc i1 + select i32
ISel                  setp.lt p0, r4, r5
                      selp   r4, p0, r4, r5
AsmPrinter            exit
```

```asm
	setp.lt p0, r4, r5
	selp    r4, p0, r4, r5
	exit
```

### `icmp eq` + `select 1/0`

InstCombine 先把 `select i1 %c, i32 1, i32 0` 改写成 `zext i1 %c`，
于是 `Select()` 走上面那条扩展路径：

```asm
	setp.eq p0, r4, r5
	movi    r1, 0
	movi    r2, 1
	selp    r4, p0, r2, r1
	exit
```

四条指令里后两条是常量物化。第 05 章之后的 peephole 有机会把它们提到
循环外；本章不做。

## 验证

```sh
$ llvm/lib/Target/GPUMin/scripts/check-ch04.sh <build>/bin
PASS 1/4: eight ALU nodes select eight different funct values
PASS 2/4: icmp + select becomes SETP + SELP with no 0/1 materialisation
PASS 3/4: the unsigned bit selects a different SETP encoding
PASS 4/4: 22 encodings match the independent encoder
OK: chapter 04 gate passed
```

第 2 项里的 `no 0/1 materialisation` 断言是"谓词模型没有被绕过"的证据：
如果哪天有人把 `SETP` 改成写 GPR 的 0/1，这条会立刻失败。

## 常见坑

1. **`i1` 不合法时 `select` 能过、`zext` 不能**。`select` 会被 legalizer
   折成 `select_cc`，本章的 custom lowering 接住了它；但 `zext i1` 会
   停在一个没有模式的节点上。两条路径都要测。
2. **忘了 `setBooleanContents`**。默认是 `AnyBooleanContent` 一类的宽松
   约定，`icmp` 的结果在某些合并里会被当成 0/-1，`SELP` 的两个源就会
   反。设成 `ZeroOrOne` 后语义固定。
3. **`select i1 %c, i32 1, i32 0` 根本到不了 `ISD::SELECT`**。InstCombine
   会把它改成 `zext`；只写 `select` 模式会得到一个"看起来没生效"的
   结果。
4. **`getMachineNode` 返回指针**。`SDValue One = getMachineNode(...)` 编译
   不过，要写 `SDValue(getMachineNode(...), 0)`。
5. **三个移位共用一个格式**。`srl` 写成 `ISD::SRA` 的模式，或者在
   `defm SHR` 里把 funct 写成 7，`shl/shr` 的测试会同时错——但错误信息
   只会说"某条指令不对"，所以门禁按函数名逐条查。
6. **`pd` 是 3 位，但槽位是 5 位**。`SETP` 的 `pd` 放在 `[25:21]`，
   编码时高两位必须是 0；如果哪天把 `p8` 加进来，这个槽位会先出事。
7. **无符号只差 `funct[3]`**。`setp.lt` = 2、`setp.ult` = 10，写成
   `setp.lt` 的 2 或 `setp.ult` 的 0xA 只差一位，肉眼很难发现；用
   `icmp ugt` 的端到端测试比对着 `.td` 数位更可靠。

## 练习

1. 把 `SELP` 的 `pd` 从 `[10:8]` 挪到 `[2:0]`，同步改编码器、解码器和
   `gpumin-encode.py`。列出哪些测试会红，说明为什么"改一个格式"比
   "加一条指令"贵。
2. 写 `define i1 @cmp(i32 %a, i32 %b) { %c = icmp slt i32 %a, %b ret i1 %c }`，
   观察返回 `i1` 时 `LowerReturn` 需要什么（提示：`i1` 已经在 `PRED` 里，
   但 `ret` 的 ABI 槽位是 GPR）。
3. 在 `Select()` 里给 `ISD::ANY_EXTEND` 也加一条路径，说明它和
   `ZERO_EXTEND` 在语义上的区别，以及为什么本章可以先不处理。
4. 用 `llc -debug-only=isel` 跑 `icmp ugt`，确认它没有在 legalizer 阶段
   被改写成交换操作数的 `ult`。如果被改写了，模式表应该如何补？
5. 给 `gpumin-encode.py` 加一个 `--from-llvm` 模式，直接读
   `llvm-mc -show-encoding` 的输出做对拍——但先想清楚：这样做之后，
   "独立实现"还剩多少价值？

## 下一章

第 05 章加控制流：`bra`（B 格式，PC 相对偏移）、`@p bra` 的条件分支、
基本块标签和 fixup。`p0-p7` 从"只被 `SELP` 读"变成"决定 warp 走哪条
路"，也是 SIMT 分歧（第 14 章）的起点。
