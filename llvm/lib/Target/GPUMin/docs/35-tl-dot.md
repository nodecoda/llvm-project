# 第 35 章：`tl.dot`

## 目标

ch30 用 32 步 rank-1 循环拼出 matmul，ch34 把它搬到了 shared。两者都在
TTIR 里**显式**写着乘法与加法。本章把整段循环收成一个 `tl.dot`：

```python
@triton.jit
def dot_kernel(a_ptr, b_ptr, c_ptr):
    pm = tl.program_id(0)
    pn = tl.program_id(1)
    rm = pm * 8 + tl.arange(0, 8)[:, None]      # [8,1]
    rn = pn * 8 + tl.arange(0, 8)[None, :]      # [1,8]
    kk_r = tl.arange(0, 32)[None, :]            # [1,32]
    kk_c = tl.arange(0, 32)[:, None]            # [32,1]
    a = tl.load(a_ptr + rm * 32 + kk_r)         # [8,32]
    b = tl.load(b_ptr + kk_c * 32 + rn)         # [32,8]
    acc = tl.dot(a, b)
    tl.store(c_ptr + rm * 32 + rn, acc)
```

launch descriptor 与参考值仍然复用 ch30 的 `e2e/matmul2d/`：同样的
4×4 blocks、同样的 1024 个 f32 输出。区别只在 IR 的形状。

退出条件（可执行）：

```sh
llvm/lib/Target/GPUMin/scripts/check-ch35.sh <llvm-bin-dir>
TRITON_OPT=<triton-opt> \
  llvm/lib/Target/GPUMin/scripts/triton-verify-all.sh <llvm-bin-dir>
```

`check-ch35.sh` 覆盖 5 项：TTIR + canonical 对拍、汇编展开形态、1024 个
输出逐位一致、IR 结构收益、以及五条形状/类型诊断。

## TTIR：真正的 `tt.dot`

Triton 3.9 自己就把矩阵乘打印成一条 op，本章不发明语法：

```mlir
%36 = arith.constant 0.000000e+00 : f32
%37 = tt.splat %36 : f32 -> tensor<8x8xf32>
%acc = tt.dot %a, %b, %37, inputPrecision = tf32 :
         tensor<8x32xf32> * tensor<32x8xf32> -> tensor<8x8xf32>
```

第三操作数是累加器。`tl.dot(a, b)` 的缺省 acc 是零 tile；写成
`tl.dot(a, b, acc)` 就把已有累加值传进去。`tt.dot` 是注册过的 Triton op，
所以 gate 里**不需要** ch34 那样的 `--allow-unregistered-dialect`。

一个细节：`triton-opt --canonicalize` 会把“标量常量 + splat”折成
`arith.constant dense<0.000000e+00> : tensor<8x8xf32>`。对拍是两侧都
canonicalize，所以 golden 与现场输出一致。

`inputPrecision = tf32` 是 Triton 的默认打印。教学后端的 ALU 只有完整
f32，不做 tf32 舍入——本章 e2e 的数据全是小整数，两种语义逐位相同。
文档保留这个字段，是为了让 TTIR 看起来和真实 Triton 一致。

## 降级：没有 MMA 的 dot

`tt.dot` 在 GPUMin 上没有硬件指令，也不需要新增指令。`emitDot` 做的事
和 ch34 手写循环完全一样：

```text
A(8x32), B(32x8)
  -> 两个 addrspace(2) window
  -> 逐元素 st.shared
  -> 一条 bar.sync
  -> 对每个输出元素 (r,c)：sum_{k<32} A[r,k] * B[k,c]
```

区别只在**循环的位置**：ch34 的 K 循环在 TTIR 里，ch35 的 K 循环在
`LLVMEmit` 里。汇编因此仍然只有 ch16 的指令：

```text
fmul=64  fadd=64  ld.shared=96  st.shared=16  bar.sync=1  mma=0
```

`64 = 2 元素/线程 × 32 步`。`check-ch35.sh` 第 2 项把这些数字和
“没有 mma”一起锁住，证明 dot 是前端抽象而不是新硬件。

## IR 变短，流量不变

| 内核 | TTIR 里的 `arith.mulf` | issued | cycles | loc_st | glb_ld |
| --- | --- | --- | --- | --- | --- |
| ch30 rank-1 | 64 | 24832 | 16608 | 25088 | 50688 |
| ch34 shared | 32 | 53632 | 41200 | 70656 | 9728 |
| ch35 dot | **0** | 54528 | 41088 | 74240 | 9728 |

`tl.dot` 没有让数字变好，也没有变坏：它就是 ch34 的 staging 加上同一套
展开，只是把 32 步循环从 IR 挪进了后端。`loc_st` 略高，是因为多出来的
acc 初始化与地址表达式；`cycles` 基本持平。

这正是本章要说明的分层：**TTIR 描述“算什么”，后端决定“怎么展开”**。
真正的硬件 dot（MMA）会改变指令面，那是目标 ISA 的事，不是前端语法的事。

## e2e 数据

`e2e/dot2d/` 复用 `e2e/matmul2d/` 的 `launch.json` 与 `expected.json`
（kernel 名改为 `dot_kernel`）。第 3 项还断言两份 `values` 完全相同，
所以“dot 和 ch30 结果一致”不是两套参考值的巧合。

## 诊断

```text
tl.dot requires 2D f32 tiles
tl.dot K dimensions do not match
tl.dot acc shape does not match a*b
tl.dot left tile must fit 4 warps x 8 lanes with at most 8 elements per thread
tl.dot takes at most three arguments
```

形状检查顺序是“先 rank/dtype，再每线程元素数，再 K 匹配，最后 acc”。
`check-ch35.sh` 的五条负例分别命中这五类错误。

## 下一步

ch36 在 dot 的结果上做 2D mask 广播与 `other=`，写出 causal attention；
ch37 把 rank-1、shared、dot 三个 matmul 放进同一张 benchmark 表。
