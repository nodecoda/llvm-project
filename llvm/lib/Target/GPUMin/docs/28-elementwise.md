# 第 28 章：元素级完整性与广播

## 目标

第 27 章把控制流和 e2e 收进了闭环，但能写的 kernel 仍然缺几块逐元素
积木：Python 的 `//`、`%`、`tl.cdiv`，常量填充 `tl.full`/`tl.zeros`，
逐元素 `tl.maximum`/`tl.minimum`，以及第 26 章明确后置的 f32 `tl.max`。
这一章补齐它们，并给出一条可执行的“逐元素 kernel 能用”的验收线。

退出条件（可执行）：

```sh
llvm/lib/Target/GPUMin/scripts/check-ch28.sh <llvm-bin-dir>
TRITON_OPT=<triton-opt> \
  llvm/lib/Target/GPUMin/scripts/triton-verify-all.sh <llvm-bin-dir>
```

`check-ch28.sh` 覆盖 5 项：TTIR/`canonicalize` 对拍、MC 往返、`llc`
选指、模拟器语义、以及四条负例诊断。

## ISA：扩展 ALU 的 funct 空间

GM32 的 `OPC_ALU` 是 R2 格式，`funct` 有 11 位，但第 4 章只用了低 4 位：

```text
add 0  sub 1  and 2  or 3  xor 4  shl 5  shr 6  sar 7
```

本章把 `funct[10:4]` 定义为扩展命名空间，先花掉四个值：

```text
0x010 sdiv   0x011 udiv   0x012 srem   0x013 urem
```

低 4 位保持 0，所以旧解码器（`funct < 8`）完全不受影响。四条指令仍是
R2 形状，编码器、反汇编器、寄存器分配都不需要新格式：

```sh
$ echo 'sdiv r4, r5, r6' | llvm-mc -triple=gpumin -show-encoding
	sdiv r4, r5, r6                         # encoding: [0x10,0x30,0x85,0x08]
```

模拟器的分派相应地从 `funct = word & 0xF` 改成 `funct = word & 0x7FF`，
旧路径按值不变。

### 语义：每种输入都有确定结果

教学后端不引入 trap 或 flag，除法语义写死为：

| 情况 | `sdiv` | `srem` | `udiv` | `urem` |
| --- | --- | --- | --- | --- |
| `b == 0` | 0 | 0 | 0 | 0 |
| `a == INT_MIN && b == -1` | `INT_MIN` | 0 | — | — |
| 负数 | 向零截断 | 符号跟被除数 | 操作数按无符号解释 | 同左 |

前三列是 C/LLVM 的 `sdiv`/`srem` 语义，模拟器 `gpumin_sim.py` 的
`sdiv32/srem32/udiv32/urem32` 逐条实现；门禁直接对拍 Python 的 floor 参考。

## 为什么不能直接用 arith.divsi

Python 的 `//` 和 `%` 是 **floor** 语义，C 的 `/` 和 `%` 是 **truncate**
（向零）。两者在异号且不能整除时不同：

```text
-7 // 3  = -3      C: -7 / 3 = -2
-7 %  3  =  2      C: -7 % 3 = -1
```

前端 `Frontend.cpp` 的 `lowerFloorDiv()` 先发 `arith.divsi`/`arith.remsi`，
再用两次比较 + 两次选择修正：

```text
q  = sdiv a, b
r  = srem a, b
q1 = select(r != 0, q - 1, q)
qf = select((a ^ b) < 0, q1, q)        # floor(a / b)

r1 = select(r != 0, r + b, r)
rf = select((a ^ b) < 0, r1, r)        # a - floor(a/b)*b
```

两个条件都是比较产生的 `i1`，`select` 已经有 i32/f32 模式，因此不需要
谓词寄存器上的布尔运算。TTIR 里就是普通的 `cmpi` + `select`：

```mlir
%q = arith.divsi %a, %b : tensor<32xi32>
%r = arith.remsi %a, %b : tensor<32xi32>
%nz = arith.cmpi ne, %r, %zero : tensor<32xi32>
%qdown = arith.subi %q, %one : tensor<32xi32>
%q1 = arith.select %nz, %qdown, %q : tensor<32xi1>, tensor<32xi32>
```

`tl.cdiv(a, b)` 复用同一条路径：

```text
ceil(a / b) = -floor(-a / b)
```

即 `SubI(0, floor_div(SubI(0, a), b))`，正负都成立。

## 除常量的陷阱：isIntDivCheap

SelectionDAG 的 DAGCombiner 默认会把 `x / 常数` 改写成 magic multiply，
其中间结果是 `i64` 的 `mulhi`。GM32 没有 32×32→64 的乘法，这条 combine
会让 `llc` 直接失败。`GPUMinISelLowering` 覆盖：

```cpp
bool GPUMinTargetLowering::isIntDivCheap(EVT VT, AttributeList Attr) const {
  return true;   // 本 ISA 有真正的整数除法，不要生成 magic multiply
}
```

DAGCombiner 尊重这个钩子（`SelectionDAG/DAGCombiner.cpp` 的
`visitSDIV`/`visitSREM`），所以 `x / 3` 在 `.s` 里保持一条 `sdiv`。
门禁第 3 项同时断言“有 `sdiv`”和“没有 `mulhi`”。

## maximum / minimum：比较加选择

`tl.maximum(a, b)` 和 `tl.minimum(a, b)` 在前端展开成：

```text
maximum(a, b) -> select(a > b, a, b)      # i32: cmpi sgt；f32: cmpf ogt
minimum(a, b) -> select(a < b, a, b)      # i32: cmpi slt；f32: cmpf olt
```

Tensor 与标量混用时，标量先 `tt.splat` 成同形 tensor；两个 tensor 必须
同形，否则报 `tensor type mismatch`。canonicalizer 会把 i32 形态折成：

```mlir
%hi = arith.maxsi %a, %c0 : tensor<32xi32>
%lo = arith.minsi %a, %c0 : tensor<32xi32>
```

f32 形态保持 `arith.cmpf ogt/olt` + `arith.select`，因为 `arith.maximumf`
的 NaN 语义与“ordered compare + select”不完全相同。本章的定义是：

| 输入 | 结果 |
| --- | --- |
| 任一操作数是 NaN | ordered 比较为假，选择右操作数 |
| 普通值 | 数值上的 max/min |

课堂里记住一句话就够：**i32 可以折成 maxsi/minsi，f32 保留 cmp+select**，
两者在 `.s` 里都是 `fsetp`/`setp` + `selp`。

## tl.full / tl.zeros

`tl.full` 和 `tl.zeros` 只接受 1D 形状，长度必须是 32 的倍数（一个 block
的线程数）。前端做最小求值：

```python
z = tl.zeros((BLOCK,), tl.int32)
f = tl.full((BLOCK,), 7, tl.int32)
```

- `evalShape1()` 只接受 `Tuple` 里的整型常量，且元素个数为 1；
- `evalDType()` 只接受 `tl.int32` 和 `tl.float32`；
- 先发标量 `arith.constant`，再 `tt.splat`。

canonical 形态是 dense 常量，门禁会检查：

```mlir
%cst = arith.constant dense<7> : tensor<32xi32>
```

非法形状（如 `(16,)`、`(0,)`）报
`tensor size must be a positive multiple of 32`。

## f32 tl.max：补上第 26 章的后置项

第 26 章只做了 i32 的 `tl.max`，f32 明确后置，因为当时没有 f32 `SELP`。
第 27 章给了 f32 select 模式，本章补上组合器：

```cpp
// LLVMEmit.cpp: combineReduction
LlvmValue *Greater = ElemTy.isF32() ? B.CreateFCmpOGT(Rhs, A)
                                    : B.CreateICmpSGT(Rhs, A);
return B.CreateSelect(Greater, Rhs, A);
```

TTIR 打印时必须按元素类型选方言：

```mlir
%red = arith.maximumf %a, %b : f32     ; f32
%red = arith.maxsi    %a, %b : i32     ; i32
```

如果误打成 `arith.maxsi %a, %b : f32`，`triton-opt` 会因为 f32 不合法
而拒绝。门禁第 1 项对 f32 reduce 的黄金文件做 `canonicalize` 对拍。

`arith.maximumf` 是 Triton 的 canonical 形态；本章实际执行的语义是
ordered compare + SELP，非 NaN 输入下两者一致，NaN 传播不在本章教学范围内。

reduction 的两阶段（每线程先归约，再经 shared memory + 双 barrier）不变，
`combineReduction()` 同时服务两段，所以 f32 max 一次覆盖两条路径。

## 广播规则

本章只定义两种广播：

| 形态 | 行为 |
| --- | --- |
| 标量 op tensor | 标量 `splat` 成 tensor，逐元素 |
| 同形 tensor op tensor | 逐元素 |
| 不同形 tensor | 报 `tensor type mismatch` |
| 2D 或 1D→1D 的“拉伸” | 不做，属于后续布局章节 |

这与 Triton 的逐元素语义一致：标量广播是 value-level 的，tensor 形状
在 TTIR 里是静态的；不同形状不是广播，而是错误。

## 逐元素 e2e

`scripts/triton/e2e/elementwise/` 把本章的 i32 路径跑成端到端程序：

```python
a = offs - 16
q = a // 3
r = a % 3
c = tl.cdiv(a, 3)
hi = tl.maximum(a, 0)
lo = tl.minimum(0, a)
z = tl.zeros((BLOCK,), tl.int32)
f = tl.full((BLOCK,), 7, tl.int32)
tl.store(out_ptr + offs, q + r + c + hi + lo + z + f)
```

`expected.json` 的 32 个值由 Python 的 `//`、`%` 和 `ceil` 参考生成，
`run_e2e.py` 在模拟器上逐字比较。canonical 黄金文件同时锁住：

- `arith.divsi/remsi` 与 floor 修正的 select；
- `arith.maxsi/minsi`；
- `dense<7>`（`tl.full` 折叠）；
- `arith.maximumf`（f32 `tl.max` 的组合器）。

## 仍然不支持的形态

本章刻意留下的边界，门禁会用负例锁住：

| 形态 | 诊断 |
| --- | --- |
| `a < b < c` 链式比较 | `chained comparisons are not supported` |
| 不同形 tensor 的 maximum/minimum | `tensor type mismatch` |
| `tl.full((16,), ...)` | `tensor size must be a positive multiple of 32` |
| `tl.cdiv` 的 f32 操作数 | `tl.cdiv requires integer operands` |

链式比较需要 `i1` 上的 `and`/`or` 或谓词布尔运算，而 GM32 的 PRED 文件
没有这些操作；强行 `zext/trunc` 会污染类型系统。它和 2D 广播一起留到
布局章节。

## 本章新增文件

| 文件 | 作用 |
| --- | --- |
| `scripts/triton/elementwise.py` | 三个 kernel：i32 逐元素、f32 min/max、f32 `tl.max` |
| `scripts/triton/elementwise.ttir` | canonical 黄金文件 |
| `scripts/triton/e2e/elementwise/` | i32 逐元素 e2e（32 个参考值） |
| `scripts/check-ch28.sh` | 5 项章门禁 |

代码落点：`GPUMinInstrFormats.td`（funct 扩展注释）、
`GPUMinInstrInfo.td`（四条指令 + 四个 Pat）、`GPUMinISelLowering`
（`isIntDivCheap`）、`gpumin_sim.py`（32 位除法语义）、
`TTIR.h/.cpp`（`DivI`/`RemI` 与 f32 `maximumf`）、`LLVMEmit.cpp`
（`sdiv/srem`、f32 max 组合器）、`Frontend.cpp`（floor 修正、`cdiv`、
`maximum/minimum`、`full/zeros`）。
