# 第 29 章：2D tensor、2D grid 与布局转换

## 目标

第 21–28 章的 kernel 都活在一维 tensor 上：`tl.arange(0, BLOCK)` 铺满
32 个线程，`offs` 是一个 `tensor<BLOCKxi32>`。真实 Triton 代码的第一块
门槛却是矩阵形状：

```python
r = tl.arange(0, 8)[:, None]      # [8, 1]
c = tl.arange(0, 8)[None, :]      # [1, 8]
offs = r * 8 + c                  # [8, 8]
x = tl.load(in_ptr + offs)
```

本章把这条路径接到 GPUMin：2D tensor、`tt.expand_dims`、
`tt.broadcast`、`tt.trans`、shared memory 转置、2D grid，以及索引算术
需要但 GM32 一直没有的整数乘法。

退出条件（可执行）：

```sh
llvm/lib/Target/GPUMin/scripts/check-ch29.sh <llvm-bin-dir>
TRITON_OPT=<triton-opt> \
  llvm/lib/Target/GPUMin/scripts/triton-verify-all.sh <llvm-bin-dir>
```

`check-ch29.sh` 覆盖 5 项：TTIR/canonical 对拍、MC 往返、`llc` 选指
（含 shared 转置与双 barrier）、模拟器语义（copy/transpose/broadcast/
2×2 grid）、以及四条负例诊断。

## 线程布局：4×8 网格

GM32 的 block 有 32 个线程，分成 4 个 warp、每 warp 8 个 lane。第 9 章
的 `sr.tid` 是 block 内的线性编号，所以：

```text
rg   = tid >> 3        # warpid, 0..3
lane = tid &  7        # laneid, 0..7
```

2D 布局把这两个硬件坐标直接映射到矩阵行列：

```text
rowsPerThread = (R == 1) ? 1 : R / 4
colsPerThread = (C == 1) ? 1 : C / 8
row(i)        = rg   * rowsPerThread + i / colsPerThread
col(i)        = lane * colsPerThread + i % colsPerThread
```

`R == 1` / `C == 1` 的那一维保持为 0，目的是让 `[R, 1]` 和 `[1, C]` 这种
“向量”在广播前只占一行或一列。对一个 `[8, 8]` tile：

```text
rowsPerThread = 2, colsPerThread = 1
每个线程持有 (2*rg, lane) 和 (2*rg+1, lane)
```

也就是 gate 里 `copy2d` 写入的 64 个值恰好覆盖整个矩阵，且每个 lane 的
两个元素在行方向上相邻，寄存器内是行主序。

这个映射不是随便挑的：

- 同一 warp 内的 8 个 lane 共享 `rg`，天然对应矩阵的一行组；
- `lane` 是行内列号，warp 内访存是 8 个连续字，和 GM32 的 LSU 粒度一致；
- 跨行组的元素不需要 warp 内交换，只有真正需要跨 lane 的转置才走 shared。

布局上限写在 `LLVMEmit.cpp`：

```cpp
constexpr unsigned LanesPerWarp = 8;
constexpr unsigned WarpsPerBlock = ThreadsPerBlock / LanesPerWarp; // 4
constexpr unsigned MaxElementsPerThread = 8;
```

因此 `R` 必须是 1 或 4 的倍数，`C` 必须是 1 或 8 的倍数，
`rowsPerThread * colsPerThread <= 8`。`32×32` 会被拒绝，诊断是
`at most 8 elements per thread`，而不是让寄存器分配器在后面爆掉。

## arange 不再物化：expand_dims 本地求值

一维 `tl.arange(0, N)` 的旧路径要求 `N % 32 == 0`，因为每个线程按
`tid, tid+32, ...` 取元素。2D 的 `arange(0, 8)` 只有 8 个元素，按旧规则
根本无法铺到 32 个线程上。

本章的做法是**延迟求值**：`MakeRange` 对长度不足 32 的 range 只记录
`(Start, End)`，不产生 LLVM 值；`ExpandDims` 消费它时，按目标 2D 布局
本地算出坐标：

```text
axis == 1:  value = start + row(i)      # [8, 1]
axis == 0:  value = start + col(i)      # [1, 8]
```

TTIR 里对应：

```mlir
%r = tt.make_range {end = 8 : i32, start = 0 : i32} : tensor<8xi32>
%r2 = tt.expand_dims %r {axis = 1 : i32} : tensor<8xi32> -> tensor<8x1xi32>
%c2 = tt.expand_dims %r {axis = 0 : i32} : tensor<8xi32> -> tensor<1x8xi32>
```

这条规则有一个刻意留下的边界：短于 32 的 `MakeRange` 只能被
`expand_dims` 消费。若它直接流向 `store`，`emit()` 会报
`a tl.arange shorter than 32 must feed [:, None] or [None, :]`。这一章
不做“把 8 元素向量散到 32 线程”的通用布局，因为那需要跨 lane 交换，
和下一节的广播规则会互相打架。

## broadcast：只在 size-1 维上复制

`tt.broadcast` 在 Triton 的 canonical 形态里是显式 op，不是隐式类型
转换。本章只实现三种来源形状：

| 源 → 目标 | 复制方式 |
| --- | --- |
| `[R, 1] -> [R, C]` | 每个源元素在寄存器里重复 `C/8 = colsPerThread` 次 |
| `[1, C] -> [R, C]` | 整块元素重复 `R/4 = rowsPerThread` 次 |
| `[1, 1] -> [R, C]` | 标量重复 `elementsPerThread` 次 |

为什么这些复制是纯本地的：`[R, 1]` 的列号恒为 0，同一 lane 的
`colsPerThread` 个列元素本来就该相同；`[1, C]` 的行号恒为 0，同一 warp
的 `rowsPerThread` 个行元素也本来就该相同。复制只发生在寄存器数组里，
没有 lane 间通信。

前端在 `lowerBinary`/`lowerCompare`/`normalizePair` 里先做形状归一：
`[8, 1]` 与 `[1, 8]` 先各自 broadcast 到 `[8, 8]`，再做 `arith.addi`。
指针 + offset 张量的形状不一致时也走同一规则，所以
`out_ptr + r * 8 + c` 能正常 lowering。

canonical 黄金文件锁住了完整链条：

```mlir
%4 = arith.muli %1, %cst : tensor<8x1xi32>       // r * 8
%5 = tt.broadcast %4 : tensor<8x1xi32> -> tensor<8x8xi32>
%6 = tt.broadcast %3 : tensor<1x8xi32> -> tensor<8x8xi32>
%7 = arith.addi %5, %6 : tensor<8x8xi32>
```

## transpose：shared memory 加两次 barrier

`.T` 和 `tl.trans(x)` 只接受两个维度都大于 1 的 2D tensor。转置必须跨
lane 交换数据：`(r, c)` 的元素要跑到 `(c, r)`，而这两个位置通常不属于
同一个线程。

实现沿用第 26 章 reduction 的形状：为每个 trans 分配一个
`addrspace(2)` 的 `[R*C x i32]` 全局窗口（名字带 `TransCounter`，
多 kernel/多转置不会撞车），然后：

```text
store x[r, c] -> shared[r, c]
bar.sync
load shared[c, r] -> y[r, c]
bar.sync
```

第二次 barrier 是为了保护 shared 窗口在下一次复用前不被提前覆盖。
gate 第 3 项直接数 `.s` 里的 `st.shared` / `ld.shared` 和至少两个
`bar.sync`。TTIR printer 固定打印 canonical 的 order：

```mlir
%11 = tt.trans %10 {order = array<i32: 1, 0>} : tensor<8x8xi32> -> tensor<8x8xi32>
```

## 2D grid：线性 blockid 的两个坐标

第 13 章的 launch 描述只有 `grid: N`，对应 `sr.blockid`（0x004）。本章
扩展为：

```json
{ "grid_dims": 2, "grid": [2, 2], "block": 32 }
```

模拟器用 `by, bx = divmod(blockid, grid_x)` 得到坐标，`blocks` 变成
`gx * gy`。ISA 侧新增两个 SYS 选择子：

```text
0x00C sr.blockidx   2D grid 的 x 坐标
0x00D sr.blockidy   2D grid 的 y 坐标
```

第 02 章预留的 `0x00B` 已经被 `bar.sync` 占用，所以这里从 `0x00C`
开始，`GPUMinInstrInfo.td` 的注释里写清了这一点。编码与往返：

```text
sr.blockidx r1    # encoding: [0x0c,0x00,0x20,0x48]
sr.blockidy r1    # encoding: [0x0d,0x00,0x20,0x48]
```

1D launch 保持旧语义：`grid_x = grid`、`grid_y = 1`，
`blockidx == blockid`，原有 e2e 不需要改 launch JSON。

前端只开放 `tl.program_id(0)` 和 `tl.program_id(1)`；`axis >= 2` 报
`chapter 29 only supports tl.program_id(0) and tl.program_id(1)`。
`GetProgramId` 在 LLVMEmit 里按 axis 选 intrinsic：

```text
axis 0 -> llvm.gpumin.blockidx -> sr.blockidx
axis 1 -> llvm.gpumin.blockidy -> sr.blockidy
```

## 补一条 mul：索引算术不是移位

2D 地址计算一上来就暴露了 GM32 的缺口：`10 * c`、`row * 16` 这类
乘法里，只有 2 的幂能 strength-reduce 成 `shl`。非 2 的幂常量乘法会
在 SelectionDAG 里展开成多次移位加法，复杂一点的形态还会落到
`__mulsi3` libcall——而本 target 没有编译器运行时。

所以本章补一条 R2 形状的整数乘法：

```text
OPC_ALU, funct = 0x014, "mul $rd, $rs1, $rs2"
低 4 位保持 0，旧的 funct < 8 解码路径不变
```

```text
mul r16, r17, r18   # encoding: [0x14,0x90,0x11,0x0a]
```

选择器只需要一条模式：

```tablegen
def : Pat<(mul i32:$rs1, i32:$rs2), (MUL $rs1, $rs2)>;
```

门禁同时断言 `.s` 里出现真实的 `mul`，而不是 `__mulsi3`。注意这条指令
只覆盖 32 位回绕乘法；有符号/无符号的高位结果（`mulhs`/`mulhu`）仍然
不在最小集合里。

## 仍然不支持的形态

| 形态 | 诊断 |
| --- | --- |
| `a[0]`、`a[1:2]` 等其它 subscript | `only supports [:, None] and [None, :]` |
| 1D tensor 的 `.T` / `tl.trans` | `tl.trans requires a 2D tensor` |
| `32×32` tile | `at most 8 elements per thread` |
| 2D tensor 上的 `tl.sum` / `tl.max` | `reductions require a 1D tensor` |
| `tl.program_id(2)` | `chapter 29 only supports tl.program_id(0) and tl.program_id(1)` |
| 行/列不是 4/8 的倍数 | `2D tensor rows must be 1 or a multiple of 4` 等 |

2D reduction 需要把“行内 lane 归约”和“跨 warp 行组归约”组合起来，
还要定义结果形状；第 30 章先用 2D 逐元素运算做分块 matmul，第 31 章再以
`keep_dims=True` 的窄入口开放 2D 归约（本章的报错文本届时随之更新）。

## 本章新增文件

| 文件 | 作用 |
| --- | --- |
| `scripts/triton/layout2d.py` | 四个 kernel：2D copy、transpose、broadcast、2D grid |
| `scripts/triton/layout2d.ttir` | canonical 黄金文件 |
| `scripts/triton/e2e/layout2d/` | 8×8 copy 的 e2e（64 个参考值） |
| `scripts/check-ch29.sh` | 5 项章门禁 |

代码落点：`IntrinsicsGPUMin.td`（blockidx/blockidy）、
`GPUMinInstrInfo.td`（`SR_BLOCKIDX`/`SR_BLOCKIDY`、`MUL` 与三个 Pat）、
`GPUMinInstrFormats.td`（funct 表注释）、`simt-model.json` 与
`gpumin_sim.py`（两个 special register、`grid_dims: 2`、`mul`）、
`TTIR.h/.cpp`（`ExpandDims`/`Broadcast`/`Trans`）、`LLVMEmit.cpp`
（4×8 布局、deferred arange、broadcast、shared transpose、2D grid）、
`Frontend.cpp`（subscript、`.T`、`tl.trans`、2D broadcast、诊断）。
