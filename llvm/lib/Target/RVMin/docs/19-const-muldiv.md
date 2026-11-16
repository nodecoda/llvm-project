# 第 19 章：常量乘除的强度削减

## 目标

第 10 章把所有乘除都交给 libcall，第 13 章给 M 扩展后变成单条指令。本章处理
第三种情况：**除数/乘数是编译期常量**。

```c
int div7(int x)   { return x / 7; }     // 不是 div，也不是 __divsi3
int mod10(int x)  { return x % 10; }
int mul3(int x)   { return x * 3; }     // slli + sub
```

完成本章后：

1. `+m` 下 `x/7` 变成魔数乘法（`mulh`），`x%10` 不再调用 `__modsi3`；
2. 无 M 下 `x*3`、`x*5`、`x*7`、`x*10` 不再调用 `__mulsi3`；
3. `minsize` 函数在 `+m` 下改用真正的 `div`/`rem`（代码更小）；
4. 在 QEMU 中验证负数和 `INT_MIN` 边界；
5. 理解“有些优化不需要目标代码”：魔数除法是通用 DAG combine，目标只要提供
   legal 的 `mulhu/mulhs`。

## 背景：三层决策

| 层 | 谁做 | 本章位置 |
| --- | --- | --- |
| `x / 2^k` | 通用 DAG combiner：移位 + 符号修正 | 已可用 |
| `x / C` | 通用 `BuildUDIV/BuildSDIV`：魔数乘法 | 第 13 章启用 M 后**自动可用** |
| `x * C` | 通用只处理 2 的幂；其余要目标 combine | 本章新增 |

关键判断在 `TargetLowering::BuildUDIV`：它要求 `MULHU`（无符号）或 `MULHS`
（有符号）是 legal 的。第 13 章把 `MUL/DIV/REM` 都做成了 TableGen pattern，所以
“魔数除法”在 `+m` 下其实已经工作，本章不需要写一行除法代码 —— 这本身是重要一课：
**先确认通用框架是否已经覆盖，再决定要不要写目标代码**。

```bash
$B/bin/llc -march=rvmin -mattr=+m -O2 div.ll -o -
```

```asm
div7:
	lui   a1, 599186
	addi  a1, a1, 1171
	mulh  a1, a0, a1
	add   a0, a1, a0
	srli  a1, a0, 31
	srai  a0, a0, 2
	add   a0, a0, a1
```

`x % 10` 会走 `x - (x/10)*10`：多一条 `mul` 和一条 `sub`。回调 `isIntDivCheap`
返回 true 时，combiner 会放弃这条路径，直接保留 `div`/`rem` —— 这是本章第二块。

## 代码地图

| 位置 | 职责 |
| --- | --- |
| `RVMinISelLowering.cpp:94-112` | M 关/开时 `MUL/DIV/REM` 的 Legal/Expand（第 13 章） |
| `RVMinISelLowering.cpp:128` | `setTargetDAGCombine(ISD::MUL)`（本章） |
| `RVMinISelLowering.cpp:196-238` | `PerformDAGCombine`：`x*C` 的移位/加减形式 |
| `RVMinISelLowering.cpp:245-249` | `isIntDivCheap`：`minsize + M` 时保留硬件除 |
| `RVMinISelLowering.h:39,43` | 两个 override 的声明 |
| `llvm/lib/CodeGen/SelectionDAG/DAGCombiner.cpp:5627-5645` | 通用 `x%C → x-(x/C)*C` 的成本判断 |
| `llvm/lib/CodeGen/TargetLowering.h:602` | `isIntDivCheap` 的默认实现（false） |

## 关键实现

### 1. 乘常量：只挑“两条指令”的形状

```cpp
APInt Plus1 = Imm + 1;
if (Plus1.isPowerOf2())
  return DAG.getNode(ISD::SUB, DL, VT, shl(Plus1.logBase2()), X);

APInt Minus1 = Imm - 1;
if (Minus1.isPowerOf2())
  return DAG.getNode(ISD::ADD, DL, VT, shl(Minus1.logBase2()), X);
```

- `C = 2^k - 1`：`(x << k) - x`，覆盖 3、7、15、31……
- `C = 2^k + 1`：`(x << k) + x`，覆盖 5、9、17……
- 2 的幂由通用 combiner 处理（直接 `slli`），这里返回空。
- 负数常量不处理：`-3x` 需要先取反，多一条指令，收益不明；`+m` 下本来就有 `mul`。

无 M 时再加一条规则：

```cpp
if (!isOperationLegal(ISD::MUL, VT) && Imm.popcount() == 2) {
  // C = 2^a + 2^b -> (x<<a) + (x<<b)
}
```

`x*10`、`x*12`、`x*20` 都落在这里。有 M 时这个形状是负优化（`addi` + `mul`
才两条），所以用 `isOperationLegal(MUL)` 把关。

### 2. 为什么用 `isOperationLegal` 而不是 `hasStdExtM()`

`RVMinTargetLowering` 没有保存 `RVMinSubtarget` 成员；而“MUL 是否 legal”正好是
第 13 章已经登记过的动作，语义上也就是“有没有 `mul` 指令”。用它做判断，
既不用改构造函数，也不会和特性开关出现两份真相。

### 3. `minsize`：把除法还给硬件

```cpp
bool RVMinTargetLowering::isIntDivCheap(EVT VT, AttributeList Attr) const {
  return VT == MVT::i32 && Attr.hasFnAttr(Attribute::MinSize) &&
         isOperationLegal(ISD::SDIV, VT);
}
```

`DAGCombiner::visitREM` 只有在该函数返回 false 时才把 `x%C` 改写成
`x-(x/C)*C`（`DAGCombiner.cpp:5630`）。返回 true 后：

```llvm
define i32 @div7_ms(i32 %x) minsize { %r = sdiv i32 %x, 7 ... }
```

```asm
div7_ms:
	addi a1, zero, 7
	div  a0, a0, a1
```

上游 RISC-V 用的是同一个策略（`RISCVTargetLowering::isIntDivCheap`，只在
`minsize` 下为 true），本章照抄这个取舍：**默认追求速度，`minsize` 追求尺寸**。

### 4. `INT_MIN` 的边界

魔数序列对 `INT_MIN` 有专门修正（`srli + add`）。`scripts/rt/muldiv.c` 用
`volatile` 输入强制运行时计算，并检查：

```c
v = (int)0x80000000u;
if (div7(v) != -306783378) return 11;
if (mod10(v) != -8)        return 12;
```

（`-2147483648 = -306783378 * 7 - 2`，`% 10` 的余数则是 `-8`。）

## 构建与验证

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop
ninja -C "$B" llc llvm-mc clang

# 1. 本章 lit（RV32I / +m / minsize / -O0 / llvm-mc 往返）
"$B/bin/llvm-lit" -v "$W/llvm/test/CodeGen/RVMin/mul-const.ll"

# 2. 全部门禁（19 个 lit + 编码 + 80 对象 e2e + -verify-machineinstrs）
"$W/llvm/lib/Target/RVMin/scripts/rvmin-verify-all.sh" "$B/bin"

# 3. QEMU：同一程序跑 rv32i 与 +m 两遍
"$W/llvm/lib/Target/RVMin/scripts/rvmin-run.sh" "$B/bin"
```

## 预期输出

无 M：

```asm
mul3:
	slli a1, a0, 2
	sub  a0, a1, a0
mul10:
	slli a1, a0, 3
	slli a0, a0, 1
	add  a0, a0, a1
```

有 M：`mul3` 同上；`mul10` 变成

```asm
mul10:
	addi a1, zero, 10
	mul  a0, a0, a1
```

`div7` 用 `mulh`，`div7_ms` 用 `div`。

门禁输出：

```text
muldiv rv32i: rvmin: ok
muldiv m: rvmin: ok
PASS: constant multiply/divide program ran under qemu-riscv32-static with and without M
```

## 常见坑

### 坑 1：以为魔数除法要自己写

`BuildUDIV/BuildSDIV` 是通用的，目标只要让 `MULHU/MULHS` legal。先打开 M 看一眼
汇编，再决定要不要写代码。

### 坑 2：`llvm-mc` 往返忘记 `-mattr=+m`

`+m` 的输出里有 `mul/mulh/div/rem`，用默认特性重新汇编会报
“instruction requires the following: 'M'”。测试里两条 RUN 行要各自带上
`-mattr`。

### 坑 3：`x % 10` 反而变长

`+m` 下 `x%10` 是 `mulh + 修正 + mul + sub`，比 `addi + rem` 长。这不是 bug：
默认成本模型假设除法很慢；要按尺寸优化时，`minsize` + `isIntDivCheap` 才会
改回 `rem`。

### 坑 4：combine 返回了不 legal 的节点

`PerformDAGCombine` 返回的 `SHL/ADD/SUB` 都在 RV32I 里天然可用；如果返回
`MUL`（比如想做 `x*C → x*(C/2)<<1`），在没有 M 时会再次落到 libcall 上。

### 坑 5：忘了 `-O0` 也会 combine

DAG combiner 与优化级别无关；`llc -O0` 的 `x*3` 同样是 `slli + sub`。想观察
“未优化”的形状只能用 `-stop-after=isel` 之前的 IR/DAG。

## 练习

1. 给负常量 `x * -3` 加一条模式：`x - (x<<2)`，并验证不引入 `__mulsi3`。
2. 把 `x*10` 的无 M 规则扩展到“任意 popcount ≤ 3”，测量代码尺寸变化。
3. 用 `-stats` 或 `-print-after=combine` 观察 `x%10` 在哪一步被改写。
4. 在 `-Oz`（clang 会设置 `minsize`）下对比 `div7` 的两种输出。
5. 给 `mul-const.ll` 增加 64 位版本，观察 i64 是否仍走 libcall。

## 下一章

第 20 章写反汇编器：让 `llvm-mc -disassemble` 与 `llvm-objdump -d` 真正使用
RVMin 自己的解码器，形成 `asm → obj → disasm → asm` 闭环。
