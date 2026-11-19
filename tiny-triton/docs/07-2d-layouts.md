# 第 7 章（上）：2-D 布局与线程坐标

前六章的所有 tensor 都是 1-D。本章第一次出现“行”和“列”：

```python
rows = tl.arange(0, 8)[:, None]     # tensor<8x1xi32>
cols = tl.arange(0, 8)[None, :]     # tensor<1x8xi32>
offs = rows * 8 + cols              # tensor<8x8xi32>
x = tl.load(in_ptr + offs)
tl.store(out_ptr + offs, x)
```

这个 kernel 只做复制，却覆盖了 2-D 教学后端要解决的全部基础问题：

- `[:, None]` / `[None, :]` 如何变成 IR（`tinytt.expand_dims`）；
- `8x1` 与 `1x8` 如何合成 `8x8`（`tinytt.broadcast`）；
- `tensor<8x8>` 如何分给 4 个 warp × 8 个 lane；
- 每个线程的 2 个元素分别落在哪个 (row, col)；
- 指针如何按 `row * 8 + col` 跨步。

本章拆成两节。本节（7a）只做坐标模型与 2-D copy；`trans`、
`convert_layout`、2-D reduce 与 softmax/matmul 在 7b。

## 1. 冻结的坐标模型

GPUMin 的 block 是 32 线程：`tid ∈ [0, 32)`。默认 2-D 布局是

```text
sizePerThread = [1, 1]
threadsPerWarp = [1, 8]
warpsPerCTA    = [4, 1]
order          = [1, 0]
```

于是：

```text
lane = tid % 8
warp = tid / 8

dim 0（行） 的线程坐标 = warp
dim 1（列） 的线程坐标 = lane
```

一个 tile 是 `4 行 × 8 列`。`8x8` 比 tile 大，所以按 `order` 重复
（`order[0] = 1` 表示列是最快变化的维度）：

```text
每个线程 2 个元素：
  (warp,     lane)      ← 第 0 次重复
  (warp + 4, lane)      ← 第 1 次重复
```

线程 `tid` 的两个线性偏移是 `warp*8 + lane` 与 `warp*8 + lane + 32`，
正好是 64 个 offset 的一半，4 个 warp 合起来覆盖 0..63。

> 旧 `gpumin-triton` 的 emitter 用的是 `warp*2 + {0,1}`（行连续），
> 本章用 tile 重复语义（`warp` 与 `warp+4`）。两者都能覆盖全张量，
> 但后者才是 `blocked` 布局在 `sizePerThread=[1,1]` 下的定义，
> 也是我们第 3 章就写进 verifier 的规则。

## 2. 三条合法性规则

`LayoutGeometry.h` 是布局算术的唯一实现（TT→TTG pass 与 TTG→LLVM pass
都调用它，避免两处公式漂移）：

```text
tile[d]     = sizePerThread[d] * threadsPerWarp[d] * warpsPerCTA[d]
repeats[d]  = shape[d] / tile[d]
threads[d]  = threadsPerWarp[d] * warpsPerCTA[d]
```

一条 tensor 维度只有三种合法形态：

| 形态 | 条件 | 语义 |
| --- | --- | --- |
| 整除 | `shape[d] % tile[d] == 0` | tile 重复 `shape[d] / tile[d]` 次 |
| 复制 | `shape[d] == 1` | 该维没有自己的线程，所有线程坐标记为 0 |
| 回绕 | `shape[d] < tile[d]` 且 `threadsInLayout % shape[d] == 0` | `tid % shape[d]` |

“复制”不是偷懒，而是 `8x1` 这种形状的**唯一**合理读法：它只有一列，
却要交给 8 个 lane。若要求 `shape >= tile`，`[:, None]` 根本无法表示。
“回绕”同理：`tl.arange(0, 8)` 只有 8 个元素，而 block 有 32 线程，
剩下的 24 个线程只能重复。

回绕形态被限制得很死：只有 `4/8/16/32` 这类能整除 block 的尺寸才被接受，
`tl.arange(0, 6)` 会被拒绝——否则“哪个线程持有第 6 个元素”没有答案。

## 3. T4：按 shape 选布局

第 4 章的 `getDefaultBlocked` 只看 rank；这一章它必须看 shape：

```text
rank 1: warps = min(4, max(1, extent / 8))
rank 2: warps(行) = min(4, max(1, rows))
```

于是 `tl.arange(0, 8)` 得到 `warpsPerCTA = [1]`（一个 warp 就够），
而 `arange(0, 1024)` 仍是 `[4]`（与第 5 章完全一致，golden 不变）。
`8x8` 得到 `warpsPerCTA = [4, 1]`：4 个 warp 各管 4 行中的 1 行，
每个 warp 内的 8 个 lane 平分 8 列。

## 4. 前端：Subscript 与 broadcast

`EmitTTIR.cpp` 新增两个能力：

1. **下标只接受两种形状**：

   ```python
   x[:, None]   ->  tinytt.expand_dims %x <axis = 1>
   x[None, :]   ->  tinytt.expand_dims %x <axis = 0>
   ```

   其他下标（真正的切片、整数下标）报错。`tinytt` 没有“取一行”的 op，
   假装支持只会让后续 pass 无法实现。

2. **二元运算补广播**：

   ```text
   r : tensor<8x1>, c : tensor<1x8>
   r * 8 + c
   ->
   %5 = tinytt.broadcast %r : tensor<8x1> -> tensor<8x8>
   %7 = tinytt.broadcast %c : tensor<1x8> -> tensor<8x8>
   %8 = arith.addi %5, %7
   ```

   规则是逐维的：相等取该值，一方为 1 取另一方，否则报出**具体维度**：

   ```text
   line 9: cannot broadcast dimension 0: 4 against 8
   ```

   `tinytt.broadcast` 只表达“从 1 长到大”，不做 stride 变换——那是
   `trans`/`convert_layout` 的职责。

## 5. T5：坐标、重算与寄存器广播

### 5.1 `coordValue`

`TTGToLLVM` 新增一个函数，把“第 i 个寄存器在第 d 维的坐标”算成 IR：

```text
linear = tid % threadsInLayout        // 布局没占满 block 时才有
coord  = (linear / stride[d]) % threads[d]
coord  = coord * sizePerThread[d] + within[d] + repeat[d] * tile[d]
```

对 8x8 的行维：`stride = 8, threads = 4` → `tid / 8`；
列维：`stride = 1, threads = 8` → `tid % 8`。
`sizePerThread = 1`、`within = 0` 时，这两个加法会被常量折叠掉，
所以最终 IR 就是：

```mlir
%16 = llvm.udiv %tid, %c8 : i32        // warp
%20 = llvm.add %16, %c4  : i32         // 第二次重复的行
%24 = llvm.urem %tid, %c8 : i32        // lane
%26 = llvm.mul %16, %c8                // warp * 8
%28 = llvm.add %26, %24                // (warp, lane)
```

### 5.2 `expand_dims` 不做跨线程搬运

`arange` 的值就是它的坐标。所以 `x[:, None]` 不需要先把 1-D 分布
搬到 2-D 分布——直接在**目标坐标**上重算：

```text
elts[i] = coordValue(resultType, i, sourceDim)
```

这是 Triton 自己的 lowering 也用的观察，代价是源 `make_range` 变成死代码
（`llc -O1` 会删掉）。好处是 7a 完全不需要 shared memory 或 shuffle。

若 `expand_dims` 的输入不是 `make_range`，pass 明确报错：

```text
chapter 7 only expands tl.arange results; a computed 1-D tensor needs a
layout conversion, which chapter 7b adds
```

### 5.3 `broadcast` 是寄存器重命名

从 `8x1` 长到 `8x8` 时，两种布局的“每线程元素数”都是 2：size-1 维本来
就把 lane 当复制用。所以广播只是把线程已经持有的寄存器排列组合：

```text
对每个目标寄存器 i：
  src_position = dst_position      // 相同 repeat
  若 src.shape[d] == 1: src.repeat[d] = 0, src.within[d] = 0
  index = linearElementIndex(srcLayout, src_shape, src_position)
  elts[i] = src.elts[index]
```

`8x1 -> 8x8` 是恒等映射；`1x8 -> 8x8` 把唯一的寄存器重复 2 次。
两者都不产生指令。

## 6. 走读产物

TTGIR 里每个 tensor 都带布局，且 8x8 的三处 tensor 共用 `#blocked1`：

```mlir
%1 = tinyttg.expand_dims %0 <axis = 1> : tensor<8xi32, #blocked> -> tensor<8x1xi32, #blocked1>
%6 = tinyttg.broadcast %5 : tensor<8x1xi32, #blocked1> -> tensor<8x8xi32, #blocked1>
%10 = tinyttg.addptr %9, %8 : tensor<8x8x!tinytt.ptr<f32>, #blocked1>, tensor<8x8xi32, #blocked1> -> ...
```

LLIR 里 tensor 全部消失，只剩 2 个 `llvm.load` / 2 个 `llvm.store`
（每个线程 2 个元素），以及用 `udiv/urem` 算出来的行列坐标。

## 7. 门禁

```sh
tiny-triton/scripts/check-t7a.sh /home/ubuntu/llvm/builds/pr-ctpop/bin
```

五项：

1. 前端产出两个 `expand_dims`、两个 `broadcast`、`addptr`、`store`；
2. TTGIR 与 golden 逐字节一致，且 2-D 布局是 `warpsPerCTA = [4, 1]`、
   1-D 小区间是 `warpsPerCTA = [1]`；
3. LLIR 与 golden 逐字节一致，且不含 `tensor<` / `tinyttg.*`；
4. `mlir-translate` + `llc -march=gpumin -verify-machineinstrs` 成功；
5. 模拟器把 `0..63` 按原顺序复制到目标缓冲（行列互换会立刻暴露）。

## 8. 退出条件

- `ninja -C <build> tiny-opt tiny-triton` 成功；
- `check-t1.sh`–`check-t7a.sh` 全绿（旧章节的 golden 逐字节不变）；
- `check-t7a.sh` 5/5；
- `docs/03` 的合法性规则表与 verifier 一致。

## 9. 下一节（7b）

`docs/07b-transpose-softmax-matmul.md` 继续：

- `tl.trans` / `.T`、`convert_layout`：经 shared memory 往返；
- `tl.exp2`、`tl.zeros/full`、2-D `tl.sum/max`（axis 1 + `keep_dims`）；
- `tl.static_range` 展开、`program_id(1)`；
- `softmax2d.py`、`matmul2d.py`（32³，2-D grid）。
