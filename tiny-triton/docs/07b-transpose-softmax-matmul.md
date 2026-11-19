# 第 7 章（下）：trans、softmax 与 matmul

7a 的所有 kernel 都能“就地”完成：每个线程只看自己寄存器里的值，
`expand_dims` 与 `broadcast` 不产生任何指令。本章第一次出现**必须跨线程
搬数据**的运算：

- `tl.trans`：元素 (r, c) 必须去持有 (c, r) 的线程；
- `convert_layout`：同一个 tensor 换一种分布；
- 行方向的 `tl.max` / `tl.sum`：一行的 8 个元素分散在 8 个 lane 上；
- `tl.exp2`：GPUMin 的 SFU 指令；
- `tl.static_range` + `program_id(1)`：把 32³ 的 matmul 写成 4×4 个 8×8 tile。

跨线程的唯一通道是 shared memory，唯一的同步是 barrier。本章把这两样
做成两个可复用的 helper，`trans`、`convert_layout`、2-D reduce 和
显式的 `local_*` 都走同一条路。

## 1. shared 是一个“块内信箱”

```cpp
Value sharedWindow(int64_t elements);  // 从 llvm.gpumin.shared 切一段新窗口
void  blockBarrier();                  // llvm.gpumin.barrier
```

约定：

1. 每次 `sharedWindow` 只把游标往**前**推，永不复用；
2. 写入的地址由元素坐标决定，而不是由寄存器编号决定：

   ```text
   index = coord[0] * shape[1] + coord[1]      // 行主序
   addr  = window + 4 * index
   ```

3. 写完之后、读之前，必须 `barrier`；因为游标单调增长，读完不需要再同步。

把“地址 = 坐标”写进代码，是本章几乎所有正确性问题的关键：两个不同布局
的线程只要算的是同一个坐标，就会读写同一个槽。

## 2. `trans`：8×8 的镜像

```python
y = tl.trans(x)      # x.T 是同一个 op
```

TTGIR 里是 `tinyttg.trans %x <order = [1, 0]>`。lowering 分两段：

```text
写：每个线程按自己的 (row, col) 把 2 个寄存器写进 shared
barrier
读：每个线程按**目标**布局的 (row, col) 读 1 个寄存器，
    读的槽是 (col, row) —— 这就是 order 的作用
```

实现上 `gatherFromShared` 接受一个 `sourceOf[]`：

```text
convert_layout:  sourceOf = [0, 1]
trans:           sourceOf = order = [1, 0]
```

`sourceOf[d]` 表示“目标第 d 维来自源的第几维”。同一个函数因此同时
服务两种 op，不需要写两遍坐标逻辑。

尺寸为 1 的维度不参与交换（它本来就是复制语义），本章直接拒绝：

```text
transposing a dimension of extent 1 is a layout change, not a transpose;
chapter 7 rejects it instead of guessing which shared slot each thread should read
```

## 3. `convert_layout`：同 shape，不同分布

`tests/ttg/convert_layout.ttgir` 是手写的 golden，用两个布局描述同一个
`tensor<8x8xf32>`：

| 布局 | 线程 | 每线程元素 |
| --- | --- | --- |
| `#blocked` | 4 warp × 8 lane | 2 |
| `#wide` | 1 warp，行交给 lane | 8 |

shape 和元素类型都一样，所以没有任何 op 能“顺手”完成这次搬运——
`convert_layout` 就是那次搬运本身。lowering 与 `trans` 共用写/读两段，
`sourceOf` 取恒等映射。

> 这个 golden 在第一次跑通前抓到了一个真实的 bug：当某一维在布局里只有
> 1 个线程（`threadsPerWarp * warpsPerCTA == 1`）时，坐标必须是 **0**，
> 而不是“线性线程号”。漏掉这条，`#wide` 会把 lane 号当成列号，
> shared 地址算成 `lane*32 + lane*4`。门禁第 2 项现在会跑这个 kernel，
> 断言目标缓冲被写成 `1.0f`。

`local_alloc` / `local_store` / `local_load` 是“显式版”的同一条路径：
`local_alloc` 分配并写入，`local_store` 写，`local_load` 读，
`barrier` 直接发指令。它们在本章只由手写 TTGIR 使用，用来证明
staging 是公开可见的，而不是 pass 的私有技巧。

## 4. `exp2`、`zeros`、`full`

GPUMin 有 `llvm.gpumin.exp2`（第 31 章的 SFU 原语），没有 libm。
所以本章新增一个逐元素 op：

```mlir
%15 = tinytt.exp2 %14 : tensor<8x8xf32> -> tensor<8x8xf32>
```

verifier 要求输入输出同类型且元素是 f32。lowering 是一个
`llvm.call_intrinsic "llvm.gpumin.exp2"`。

`tl.zeros((8, 8), tl.float32)` 与 `tl.full((8, 8), v, tl.int32)` 是
编译期描述符：shape 与 dtype 直接从前端 AST 读，不进 IR，最后落成

```mlir
%cst = arith.constant 0.000000e+00 : f32
%6 = tinytt.splat %cst : f32 -> tensor<8x8xf32>
```

没有新 op，也没有“tensor 常量”。

## 5. 2-D reduce：axis 1 与 keep_dims

一行的 8 个元素在同一个 warp 的 8 个 lane 上，所以行内 reduce 是
**warp 内**的归约。本章不引入 shuffle，统一走 shared：

```text
1. 把整块 [R x C] 写进 shared（行主序），barrier
2. 每个线程读出自己负责的那一行的 C 个值，在寄存器里归约
```

结果类型是 `tensor<R x 1>`，由 `keep_dims=True` 决定：

```python
m = tl.max(x, axis=1, keep_dims=True)   # tensor<8x1xf32>
```

`keep_dims=False` 被拒绝：

```text
chapter 7 keeps the reduced dimension: pass keep_dims=True
```

理由与 7a 的“复制语义”一致：`tensor<8>` 的默认 1-D 布局是“每个线程一个
元素”，而行归约的结果天然落在“每个 warp 一行”上。硬做就会得到第二种
1-D 布局约定，收益不值得。

combine 支持 `add`、`max`、`min`：`add` 用 `fadd`，`max/min` 用
`fcmp + select`（GPUMin 没有 fmax 指令）。

## 6. `tl.static_range`：编译期展开

```python
for k in tl.static_range(0, 8):
    ...
```

前端在 AST 层展开：循环变量变成 `arith.constant`，循环体被逐条 lower。
没有 SCF，也没有分支。约束很明确：

- 边界必须是常量；
- 迭代次数 ≤ 64；
- `range()` 在边界为常量时等价于 `static_range`。

数据相关的循环报：

```text
chapter 7 only unrolls loops with constant bounds
```

`acc += expr` 也是本章新增的语句形式（Python 的 `AugAssign`）。

## 7. `program_id(1)` 与 2-D grid

第 5 章的 `program_id` 只支持 axis 0（`llvm.gpumin.blockidx`）。
本章打开 axis 1：

```text
axis 0 -> llvm.gpumin.blockidx
axis 1 -> llvm.gpumin.blockidy
```

机器侧 `sr.blockid` 仍是线性的 `blockidy * gridX + blockidx`，所以 1-D
kernel 的语义完全不变。模拟器的 launch 描述里写 `"grid_dims": 2`。

## 8. softmax

```python
x = tl.load(in_ptr + rows * 8 + cols)     # [8,8]
m = tl.max(x, axis=1, keep_dims=True)     # [8,1]
e = tl.exp2(x - m)                        # [8,8]
s = tl.sum(e, axis=1, keep_dims=True)     # [8,1]
y = e / s
```

这是“以 2 为底的 softmax”：

```text
y[r, c] = 2 ** (x[r, c] - max_r) / sum_c 2 ** (x[r, c] - max_r)
```

命令行里没有 `exp`，所以教程选择了 GPUMin 真正能算的那个函数，而不是
假装有 libm。门禁用同一公式的 Python 参考做逐元素对比（误差 < 1e-5），
并断言汇编里出现 `exp2` 与 `bar.sync`。

## 9. matmul：没有 dot 的 32³

本章**不**实现 `tl.dot`。32³ 的 matmul 用一个 8×8 tile、4×4 的 2-D grid
和 8 次外积完成：

```python
acc = tl.zeros((8, 8), tl.float32)
for k in tl.static_range(0, 8):
    a = tl.load(a_ptr + (pid_m * 8 + rows) * 32 + k)   # [8,1]  A[:, k]
    b = tl.load(b_ptr + cols + (k * 32 + pid_n * 8))   # [1,8]  B[k, :]
    acc += a * b                                       # [8,8] 外积
```

三个要点：

1. `acc += a * b` 是 `[8,1] × [1,8] -> [8,8]` 的广播，7a 的
   `expand_dims/broadcast` 正好够用；
2. `A[:, k]`、`B[k, :]` 是通过**指针**取出来的（行偏移随 `k` 变化），
   不需要对寄存器切片——`tinytt` 没有切片 op，也不需要；
3. 指针表达式必须把 tensor 偏移写在前面：`b_ptr + cols + scalar`。
   `b_ptr + scalar` 是“标量指针 + 标量偏移”，不是 tensor，前端会拒绝。

一次性把 8 个 `k` 展开后，每个线程做 8 次 `fmul` + `fadd`，共 16 个
寄存器，汇编里没有任何循环。门禁把 A 填成 `A[i][k] = 32i + k`、B 填成
全 1，于是

```text
C[i][j] = sum_k (32i + k) = 256i + 28
```

1024 个结果必须逐位等于这个式子；行列转置、偏移错误或漏掉某个 k 都会
立刻失败。

## 10. 门禁

```sh
tiny-triton/scripts/check-t7b.sh /home/ubuntu/llvm/builds/pr-ctpop/bin
```

五项：

1. `transpose2d` 在模拟器上把 `0..63` 变成 `(i%8)*8 + i/8`，
   TTGIR 含 `tinyttg.trans`，LLIR 与 golden 一致且含 `ptr<2>` + barrier；
2. 手写 `convert_layout.ttgir` 降级与 golden 一致，LLIR 里 shared 写 ≥ 2、
   读 ≥ 8，并且真的跑出 `1.0f`；
3. `softmax2d` 与基 2 参考一致，汇编含 `exp2` 与 `bar.sync`；
4. `matmul2d` 在 4×4 的 2-D grid 上算出 1024 个 `256i + 28`；
5. 三种非法形态（2-D reduce 缺 `keep_dims`、不可广播的 shape、
   宽度为 1 的 `trans`）都被拒绝且诊断正确。

## 11. 退出条件

- `ninja -C <build> tiny-opt tiny-triton` 成功；
- `check-t1.sh`–`check-t7b.sh` 全绿；
- `gpumin-verify-all.sh` 8/8、`triton-verify-all.sh` 无失败；
- `docs/03` 的合法性规则表、`docs/05` 的边界清单与实现一致；
- 下一章 `docs/08-pipeline-and-resume.md` 把三级 checkpoint 变成可续跑的 CLI。
