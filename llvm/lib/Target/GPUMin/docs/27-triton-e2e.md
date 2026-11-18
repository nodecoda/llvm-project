# 第 27 章：控制流、e2e 与第二课收束

## 目标

第 21–26 章把 `kernel.py -> TTIR -> LLVM IR -> GM32 ELF -> sim` 的每一层
单独立起来了。这一章做三件事，把第二课收成一个可演示、可对拍的闭环：

1. 表达式级选择：`tl.where(cond, x, y)`；
2. 编译期循环：`tl.static_range(...)` 与 `range(...)` 展开成常量；
3. 四个 e2e 程序：i32 vecadd、f32 vecadd、reduction、32×32×32 matmul。

退出条件（可执行）：

```sh
llvm/lib/Target/GPUMin/scripts/check-ch27.sh <llvm-bin-dir>
TRITON_OPT=<triton-opt> \
  llvm/lib/Target/GPUMin/scripts/triton-verify-all.sh <llvm-bin-dir>
```

## tl.where：一条 select，两种条件形状

`tl.where` 不做分支。它和 Triton 一样是**逐元素选择**：

```python
z = tl.where(x < y, x, y)
```

TTIR 先产生 `arith.cmpi`，再产生 `arith.select`。关键在打印规则：

```mlir
%7 = arith.cmpi slt, %x, %y : tensor<32xi32>
%z = arith.select %7, %x, %y : tensor<32xi1>, tensor<32xi32>
```

`arith.select` 的标量条件类型默认是 `i1`；tensor 条件必须显式写出
`tensor<...xi1>`，再跟结果类型（`mlir/lib/Dialect/Arith/IR/ArithOps.cpp` 的
`SelectOp::parse`）。漏掉条件类型时 `triton-opt` 会报
`use of value '%8' expects different type than prior uses: 'i1' vs 'tensor<32xi1>'`。

LLVM emitter 把 select 逐元素展开：

```text
tensor<32xf32>  ->  32 个标量 select
i1 条件          ->  谓词寄存器 p0
```

GM32 侧只需要 TableGen 的一条模式：

```tablegen
def : Pat<(select i1:$pd, i32:$rs1, i32:$rs2), (SELP $pd, $rs1, $rs2)>;
def : Pat<(select i1:$pd, f32:$rs1, f32:$rs2), (SELP $pd, $rs1, $rs2)>;
```

因为 i32/f32 共享 `GPR`，`SELP` 搬的是 32 位模式，不需要类型转换。

## 编译期循环：把 CFG 留给以后

第二课的 TTIR-min 是单函数、直线 SSA：`TTIR.h` 的 `OpKind` 没有 Block、
Branch 或 Phi。与其在 capstone 里引入一套 CFG，这一章把循环限制在
**编译期可数**的形态：

```python
for i in tl.static_range(0, 5):
    acc = acc + i
for j in range(1, 4):
    acc = acc + j
```

前端在 `Frontend.cpp` 里求值 `start/end/step`，然后直接展开循环体：
循环变量在每一轮是一个新的 `arith.constant`，SSA 环境重新绑定。于是
上面的两段循环在 TTIR 里就是 8 条 `arith.addi`，没有 `scf.for`，也没有
`cf.br`；`triton-opt --canonicalize` 还能把它折成：

```mlir
%c16_i32 = arith.constant 16 : i32
tt.store %arg0, %c16_i32 : !tt.ptr<i32>
```

限制是显式的：

| 形态 | 行为 |
| --- | --- |
| `tl.static_range(a, b[, step])` | 编译期展开 |
| `range(a, b[, step])` | 同样编译期展开（本课不产生运行时循环） |
| `for/else` | 报错 `for/else is not supported` |
| 迭代次数 > 1024 | 报错 `static range is limited to 1024 iterations` |
| `while` | 报错 `unsupported statement 'While'` |

> 边界：运行时 `if/else` 与 `while` 需要先给 TTIR 建 CFG；第三课收束时仍未开放，
> 这是刻意留下的空白（见第 32 章的“仍然不做”表）。
> 本章的选择语义由 `tl.where` 覆盖。

## 四个 e2e 程序

`scripts/triton/e2e/` 下每个目录都是 `kernel.py + launch.json + expected.json`，
由 `run_e2e.py` 统一编译、运行和比较：

| 程序 | kernel | 输入 | 期望 |
| --- | --- | --- | --- |
| `add_i32` | `add_kernel` | `x=[0..255]`，`y=[1000..1255]` | 前 8 个 `1000,1002,...,1014` |
| `add_f32` | `f32_add` | `x=1.5`，`y=2.25` | `3.75` |
| `reduce` | `reduce_sum` | block 0 全 1，block 1 全 2 | `32 64` |
| `matmul_no_dot` | `matmul_kernel` | `A[r][k]=r+1`，`B=1` | 行 r 全为 `32*(r+1)` |

f32 的 dump 是原始 32 位模式，`expected.json` 存人类可读的浮点值，
`run_e2e.py` 用 `struct` 把模拟器的整数 word 转回 f32 后逐位比较。

### matmul：不用 tl.dot 的 32×32×32

```python
row = tl.program_id(0)
col = tl.arange(0, 32)
acc = 0.0
for k in tl.static_range(0, 32):
    a = tl.load(a_ptr + row * 32 + k)
    b = tl.load(b_ptr + k * 32 + col)
    acc = acc + a * b
tl.store(c_ptr + row * 32 + col, acc)
```

- 一个 block 负责一行，32 个 lane 负责 32 列；
- `a` 是标量 load，`b` 是向量 load，`a * b` 由前端 splat 成 `tensor<32xf32>`；
- k 循环展开 32 次，只有 `fmul/fadd`，没有 `tl.dot`、没有 shared tile；
- `row * 32` 与 `k * 32` 都是 2 的幂，LLVM 会折成 shift，避开当前整数
  `mul` 的 ISel 缺口。

## 两层门禁

**章门禁 `check-ch27.sh`（5 项）**

1. `tl.where` 产生带显式 `tensor<i1>` 条件的 `arith.select`，且 canonical
   与 `control_flow.ttir` 一致；
2. `static_range`/`range` 在 TTIR 里只剩常量，sim 上展开求和为 16；
3. i32/f32 where 的汇编都出现 `selp`，运行时结果正确；
4. 四个 e2e 与参考 JSON 一致；
5. `while`、`for/else`、超长 range、非 i1 的 where 条件都被拒绝。

**总门禁 `triton-verify-all.sh`（T1–T7）**

| 层 | 内容 |
| --- | --- |
| T1 | AST 识别 4 个控制流 kernel 与 `matmul_kernel` |
| T2 | 5 个 TTIR 模块都被 `triton-opt` 解析 |
| T3 | `add_i32`/`f32_add`/`reduce_sum`/`control_flow` 与 golden 一致 |
| T4 | 布局 pass 得到 `gpumin:1`、`threadsPerWarp=[8]`、`warpsPerCTA=[4]` |
| T5 | 5 个 kernel 过 `llc -verify-machineinstrs` |
| T6 | 4 个 e2e 数值一致 |
| T7 | 4 条负例诊断可操作 |

T2/T3/T4 在找不到 `triton-opt` 时报告 SKIP；其余必须 PASS。

## 已知边界

- 无运行时 `if/else`、无 `while`、无动态 trip count；
- 无 `tl.dot`、无 2D tensor、无 shared GEMM tile；matmul 是教学版；
- 整数 `mul`（非常量、非 2 的幂）仍会触发 ISel 缺口，matmul 因此用 f32；
- 循环展开上限 1024，超过时前端报错而不是悄悄截断。

## 验收输出

```text
PASS 1/5: tl.where is a typed arith.select and canonicalises cleanly
PASS 2/5: static_range and range unroll to constants and yield 16
PASS 3/5: i32/f32 where reach SELP and produce the right values
PASS 4/5: add_i32, add_f32, reduce and matmul_no_dot match their references
PASS 5/5: while, for/else, long ranges and bad where conditions are rejected

Chapter 27: 5/5 checks passed
```
