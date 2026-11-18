# 第 30 章：2D 分块 matmul（rank-1 update，不用 `tl.dot`）

## 目标

第 29 章把 2D tile、broadcast、shared 转置和 2D grid 接进了后端，但还没有
一个真正的 2D kernel。本章用它们拼出 32×32×32 的 f32 矩阵乘法，并且
**不使用 `tl.dot`**：

```python
@triton.jit
def matmul2d_kernel(a_ptr, b_ptr, c_ptr):
    pm = tl.program_id(0)
    pn = tl.program_id(1)
    rm = pm * 8 + tl.arange(0, 8)[:, None]   # [8,1]
    rn = pn * 8 + tl.arange(0, 8)[None, :]   # [1,8]
    acc = tl.zeros((8, 8), tl.float32)
    for k in tl.static_range(0, 32):
        a = tl.load(a_ptr + rm * 32 + k)     # [8,1]
        b = tl.load(b_ptr + k * 32 + rn)     # [1,8]
        acc = acc + a * b                    # rank-1 update
    tl.store(c_ptr + rm * 32 + rn, acc)
```

退出条件（可执行）：

```sh
llvm/lib/Target/GPUMin/scripts/check-ch30.sh <llvm-bin-dir>
TRITON_OPT=<triton-opt> \
  llvm/lib/Target/GPUMin/scripts/triton-verify-all.sh <llvm-bin-dir>
```

`check-ch30.sh` 覆盖 5 项：TTIR/canonical 对拍、ISA 面不变、`llc` 选指、
模拟器 1024 个 f32 结果、以及 `tl.dot` 与非法 2D zeros 的诊断。

## 分块：4×4 个 block，每个 8×8

32×32 输出被切成 4×4 个 8×8 tile，`program_id(0/1)` 直接选 tile：

```text
pm, pn           block 的 tile 坐标
rm = pm*8 + r    C 的行号，r ∈ [0,8)
rn = pn*8 + c    C 的列号，c ∈ [0,8)
```

每个 tile 仍然使用第 29 章的 4×8 线程布局：`warpid` 选行组、`laneid`
选列，2 个元素/线程。整个 kernel 没有任何新的布局规则。

`tl.zeros((8, 8), tl.float32)` 是本章唯一的新前端形状：`evalShape` 现在
接受 1 元或 2 元 tuple；一维仍要求 32 的倍数，二维直接交给后端布局检查
（行是 1 或 4 的倍数，列是 1 或 8 的倍数，元素/线程不超过 8）。后端
`tt.splat` 本来就能处理任意 2D 形状，所以 LLVMEmit 不需要改。

## rank-1 update：一次外积

第 k 步只做三件事：

```text
a[k]  : A 的第 k 列，形状 [8,1]
b[k]  : B 的第 k 行，形状 [1,8]
acc  += a[k] * b[k]          # [8,8] 的外积
```

`a * b` 正好命中第 29 章的广播路径：

```mlir
%31 = tt.broadcast %a : tensor<8x1xf32> -> tensor<8x8xf32>
%32 = tt.broadcast %b : tensor<1x8xf32> -> tensor<8x8xf32>
%33 = arith.mulf %31, %32 : tensor<8x8xf32>
%34 = arith.addf %acc, %33 : tensor<8x8xf32>
```

`tl.static_range(0, 32)` 在编译期完全展开，所以 TTIR 里有 32 组
`load -> broadcast -> mulf -> addf`，没有循环控制流。canonical 黄金文件
同时锁住了 `tl.zeros` 折叠成的：

```mlir
%cst = arith.constant dense<0.000000e+00> : tensor<8x8xf32>
```

## 广播是免费的，load 不是

rank-1 update 的算术都在寄存器里完成，但两个 load 带有明显的放大：

| 张量 | 形状 | 每线程元素 | 线程间重复 | 每 k 的 lane load 数 |
| --- | --- | --- | --- | --- |
| `a` | `[8,1]` | 2 | 同一行的 8 个 lane 读同一个值 | 64 |
| `b` | `[1,8]` | 1 | 4 个 warp 读同一行 | 32 |
| 合计 | | | | 96 |

而真正不同的数据只有 A 的 8 个 + B 的 8 个 = 16 个，所以每个 k 的
lane-load 放大是 6×。32 个 k、16 个 block 之后，总共约 49k 次全局
lane load。

这不是 bug，而是“寄存器广播”和“访存合并”是两件事的直接证据：
`tt.broadcast` 只改变寄存器里的布局，不会让 LSU 自动只发一次请求。
第 31 章的 shared tile 就是为这类重复 load 准备的反例。

## 寄存器压力：32 步展开的代价

布局层面每个线程只持有 2 个累加器、2 个 A 元素和 1 个 B 元素，看起来
远低于 29 个可用 GPR。但 32 步完全展开后，机器调度器会把多轮 load
提前，寄存器分配器开始在 private memory 里做溢出：

```text
$ llc -march=gpumin -O1 -verify-machineinstrs matmul2d.ll -o matmul2d.s
$ grep -c 'ld.local\|st.local' matmul2d.s
98
```

模拟器统计（16 个 block、32 线程）：

```text
issued=24832 lane_instrs=198656 loads=75776 stores=26112 cycles=16608
glb_ld=50688 shm_ld=0 loc_ld=25088 glb_st=1024 loc_st=25088
```

和 ch27 的 1D row-per-block matmul 对照：

| 版本 | issued | glb_ld | loc_ld | cycles |
| --- | --- | --- | --- | --- |
| ch27 1D（每 block 一行） | 23936 | 68608 | 8192 | 14880 |
| ch30 2D tile（8×8/block） | 24832 | 50688 | 25088 | 16608 |

结论很清楚：2D 分块把全局 load 从 68k 降到 51k，但 32 步展开让 local
traffic 从 8k 涨到 25k，周期反而略差。`loc_ld`/`loc_st` 里一部分是
callee-saved `r20..r27` 的保存/恢复，其余是展开后的真实溢出。

本章不掩盖这个代价，也不在这里修它：要真正消掉溢出，需要运行时循环
（TTIR-min 还没有 CFG）、更小的展开深度，或把 A/B 先落到 shared 再读。
这三条都是后续章节的题目。

## f32 的 launch 填充

之前的 `fill` 只接受整数 bit pattern：

```json
{ "addr": 0, "start": 0, "step": 1, "count": 32 }
```

对 f32 数据，连续浮点值 `0.0, 1.0, 2.0, ...` 的 bit pattern 不是线性的，
没法用整数 `step` 表达。本章给 fill 增加一个可选的格式：

```json
{ "addr": 0, "start": 0, "step": 1, "count": 32, "format": "f32" }
```

模拟器按 `f32(start + i*step)` 计算每个字，默认仍然是 `i32`，旧 launch
文件不受影响。

e2e 用两组规则数据：

```text
A[i][k] = 3*i + k
B[k][j] = k + 2*j
C[i][j] = 1488*i + 192*i*j + 10416 + 992*j
```

所有中间值都小于 2^24，f32 可以精确表示；1024 个期望值与模拟器逐字相等，
不会把浮点舍入混进正确性判断。

## 仍然不支持的形态

| 形态 | 诊断 |
| --- | --- |
| `tl.dot(x, y)` | `tl.dot is not supported; chapter 30 uses a rank-1 update loop instead` |
| `tl.zeros((32, 32), ...)` | `2D tensor needs at most 8 elements per thread, got 32` |
| 列数不是 8 的倍数 | `2D tensor columns must be 1 or a multiple of 8, got 9` |
| 运行时 K 循环 | 前端仍只展开 `range` / `tl.static_range` |
| A/B shared tile | 需要 `a_tile[:, k]` 列切片，ch29 未开放 |

## 本章新增文件

| 文件 | 作用 |
| --- | --- |
| `scripts/triton/matmul2d.py` | rank-1 update 内核 |
| `scripts/triton/matmul2d.ttir` | canonical 黄金文件 |
| `scripts/triton/e2e/matmul2d/` | 4×4 grid、1024 个 f32 参考值 |
| `scripts/check-ch30.sh` | 5 项章门禁 |

代码落点：`Frontend.cpp`（2D `evalShape`、`tl.dot` 诊断）、
`gpumin_sim.py`（`fill` 的 `format: f32`）、
`scripts/triton-verify-all.sh` 与 `scripts/gpumin-verify-all.sh`。
