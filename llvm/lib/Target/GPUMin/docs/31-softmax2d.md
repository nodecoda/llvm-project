# 第 31 章：2D 归约与逐行 softmax

## 目标

第 29 章接进了 2D tile 和 broadcast，第 30 章用它们拼出了 matmul，但
2D 张量还只能做逐元素运算：`tl.sum`/`tl.max` 仍然只接受 1D。本章补上
**带 `keep_dims=True` 的 2D 归约**，并用它实现逐行 softmax：

```python
@triton.jit
def softmax_kernel(x_ptr):
    r = tl.arange(0, 8)[:, None]
    c = tl.arange(0, 8)[None, :]
    x = tl.load(x_ptr + r * 8 + c)      # [8,8]
    m = tl.max(x, axis=1, keep_dims=True)          # [8,1]
    e = tl.exp2((x - m) * 1.4426950408889634)      # [8,8]
    s = tl.sum(e, axis=1, keep_dims=True)          # [8,1]
    y = e / s                                      # [8,8]
    tl.store(x_ptr + r * 8 + c, y)
```

退出条件（可执行）：

```sh
llvm/lib/Target/GPUMin/scripts/check-ch31.sh <llvm-bin-dir>
TRITON_OPT=<triton-opt> \
  llvm/lib/Target/GPUMin/scripts/triton-verify-all.sh <llvm-bin-dir>
```

`check-ch31.sh` 覆盖 5 项：TTIR/canonical 对拍、`fdiv`/`exp2` 汇编往返、
`llc` 的 shared/barrier/选指、模拟器 64 个 f32 结果，以及
`keep_dims`/`axis`/`exp2`/整数除法的诊断。

## `tt.reduce` 的形状：丢维 + `expand_dims`

Triton 的 `tt.reduce` 永远**丢掉**被归约的那一维，`keep_dims=True` 不是
一个reduce 选项，而是紧随其后的 `tt.expand_dims`：

```mlir
%13 = "tt.reduce"(%x) <{axis = 1 : i32}> ({
  ^bb0(%a: f32, %b: f32):
    %v = arith.maximumf %a, %b : f32
    tt.reduce.return %v : f32
  }) : (tensor<8x8xf32>) -> tensor<8xf32>
%m = tt.expand_dims %13 {axis = 1 : i32} : tensor<8xf32> -> tensor<8x1xf32>
```

本章的前端只开放这一条窄入口：2D 归约必须写 `keep_dims=True`，`axis`
只能是 0 或 1。这样后端只需要把归约结果按既有的 `[R,1]` 或 `[1,C]`
布局放进寄存器，不需要通用的 rank-1 reshape。

`tt.expand_dims` 在 LLVMEmit 里对“归约结果”是恒等操作：归约时
`Env[result]` 已经是展开后的元素顺序（`axis=1` 时长度 `Rows`，`axis=0`
时长度 `Cols`）。前置检查会拒绝把 2D 归约结果直接喂给别的 op：

```text
a 2D reduction result must feed expand_dims (keep_dims=True)
```

## 2D 归约怎么做：一个共享窗口 + 两次屏障

`[8,8]` 的线程布局是 4 warp × 8 lane、2 元素/线程。归约轴上的数据分布
在不同线程里，寄存器之间没法直接相加，所以 `emitReduce2D` 用最直接的
办法：

```text
1. 整个 [R,C] tile 写进 addrspace(2) 的 [R*C] 全局窗口
2. bar.sync
3. 每线程按归约轴串行累加自己负责的输出
4. bar.sync
```

两种轴的输出形状正好对上既有布局：

| 归约轴 | 每线程输出 | 结果布局 | 读取方式 |
| --- | --- | --- | --- |
| `axis=1` | `Rows` = 2 个行和/最大值 | `[8,1]` | 自己的行，列 0..C-1 |
| `axis=0` | `Cols` = 1 个列和/最大值 | `[1,8]` | 自己的列，行 0..R-1 |

`axis=0` 的 4 个 warp 会重复计算同样的 8 个列和，这是有意为之：比起
跨 warp shuffle，多算 4 倍的 8 元素加法更简单，而且结果顺序天然等于
`[1,8]` 布局。

本章的 softmax 只用 `axis=1`；`axis=0` 走同一条 `emitReduce2D` 分支，
由 `Frontend.cpp` 的 axis 检查和布局断言共同约束。

每个归约都会新建一个内部全局窗口（`__gpumin_red2d_<fn>_<n>`，`addrspace(2)`），
所以两次归约互不覆盖。代价是整块 tile 被**每个线程重复写**：`axis=1`
时同一行被 8 个 lane 写同样的值。数据量只有 64 个 f32，本章接受这个
冗余；真正需要省 shared 流量时，应改成“只写自己负责的元素”。

模拟器统计（单个 block、32 线程）：

```text
issued=756 lane_instrs=6048 cycles=602 barriers=4
alu=536 lsu=204 bar=16 glb_ld=96 shm_ld=1024 shm_st=128
```

`barriers=4` 就是两次归约各两次 `bar.sync`；`shm_ld=1024` 是 32 个线程
× 两次归约 × 每次 8 个元素再乘上 warp 内的重复读。

## 两条新指令：`fdiv` 与 `exp2`

softmax 需要两个通用 ALU 给不出的操作。本章不引入 libcall，而是把
它们变成真正的 ISA 指令：

| 指令 | 编码 | 形状 | 作用 |
| --- | --- | --- | --- |
| `fdiv rd, rs1, rs2` | opcode `0x07`（FALU），funct `3` | R2 | IEEE-754 单精度除法 |
| `exp2 rd, rs1` | opcode `0x0B`（SFU），funct `0` | R1 | 2 的幂 |

```text
fdiv r16, r17, r18    ->  [0x03,0x90,0x11,0x1e]
exp2 r16, r17         ->  [0x00,0x00,0x11,0x2e]
```

`fdiv` 加在 ch25 的 FALU funct 0/1/2 之后，`exp2` 使用新 opcode `0x0B`，
R1 形式里 `rs2` 为零、funct 选函数（`lg2`/`rcp` 留给后续）。TableGen
里各有一行 pattern：

```tablegen
def : Pat<(f32 (fdiv f32:$rs1, f32:$rs2)), (FDIV $rs1, $rs2)>;
def : Pat<(f32 (int_gpumin_exp2 f32:$rs1)), (EXP2 $rs1)>;
```

`exp2` 只能通过 intrinsic `llvm.gpumin.exp2` 到达，所以 pattern 直接匹配
intrinsic，而不是某个通用 DAG 节点。

前端同步补了两条语义：

- 浮点的二元运算从 `+ - *` 扩到 `+ - * /`；`/` 在 float 上调 DivF；
- 整数上的 `/` 被明确拒绝：Python 的 `/` 是真除法，静默当成整数除法会
  和源语言不一致，整数除法继续写 `//`。

```text
true division '/' requires f32 operands; use '//' for integer division
```

## 为什么是 `exp2` 和 `1.4426950408889634`

SFU 只实现底数 2。自然指数用换底公式：

```text
exp(x) = 2^(x * log2(e)),   log2(e) = 1.4426950408889634
```

所以内核写的是：

```python
e = tl.exp2((x - m) * 1.4426950408889634)
```

TTIR 里就是一条 `arith.mulf`（乘 `dense<1.442695e+00>` 的 splat）加一条
`math.exp2`：

```mlir
%19 = arith.mulf %16, %18 : tensor<8x8xf32>
%e  = math.exp2 %19 : tensor<8x8xf32>
```

减去行最大值是数值稳定的常规做法，也让最大元素对应 `exp2(0) = 1`。

## 与 ch26 的 1D 归约对照

| | ch26 1D 归约 | ch31 2D 归约 |
| --- | --- | --- |
| 数据位置 | 每线程 1 个元素 | 每线程 2 个元素 |
| 交换方式 | 共享窗口 + `bar.sync` | 共享窗口 + `bar.sync` |
| 归约顺序 | 线程内先加，再跨线程 | 每线程读整行/整列串行累加 |
| 输出 | 标量 | `[R,1]` 或 `[1,C]` |
| 前端入口 | `axis=0` | `axis∈{0,1}` 且必须 `keep_dims=True` |

两者都复用 `combineReduction`：`sum` 走 `fadd`/`add`，`max` 在 f32 上
走 `fsetp.ogt + selp`（asm 里可以看到 14 条 `fsetp`/`selp`：两次归约各
7 次比较）。

## e2e 数据与逐位比较

8×8 输入每行一条不同的等差序列：

```text
row i: start = (i-4)*1.5,  step = 0.25*(i+1)
```

参考值在生成 `expected.json` 时按模拟器同样的单精度语义逐步舍入：
`f32(x-m)`、`f32(*log2e)`、`f32(2^t)`、从左到右的 `f32` 加和、`f32(e/s)`。
64 个输出与模拟器**逐位相等**，并且每行和都是 `1.000000`。

`run_e2e.py` 不做容差比较，所以这组数据同时锁住了：

- 行归属（8 行模式各不相同，错行立刻暴露）；
- `fdiv` 的舍入；
- `exp2` 的换底常数；
- 两次归约的屏障顺序。

## 仍然不支持的形态

| 形态 | 诊断 |
| --- | --- |
| 2D 归约不带 `keep_dims` | `chapter 31 requires keep_dims=True for 2D reductions` |
| `axis=2` | `2D reductions reduce axis 0 or 1` |
| `tl.exp2` 作用在非 f32 张量 | `tl.exp2 requires an f32 tensor` |
| 整数 `/` | `true division '/' requires f32 operands; use '//' for integer division` |
| 2D 归约结果直接参与运算 | `a 2D reduction result must feed expand_dims (keep_dims=True)` |
| `tl.exp` / `tl.log` / `tl.rsqrt` | 本章只有 `exp2`；SFU 其它 funct 留给后续 |

已知边界：

- `axis=0` 已实现但没有 e2e 覆盖，只有 axis 检查和布局断言；
- 每个归约重复写整块 tile，shared 流量有冗余；
- 模拟器的 SFU 和 ALU 共用 `alu` issue slot（`simt-model.json` 未加
  `sfu` 单元），所以 `alu` 计数包含 `exp2`；
- 模拟器用 double 计算再舍入到 f32，理论上存在 double-rounding 边界，
  本章的测试向量避开了它。

## 本章新增文件

| 文件 | 作用 |
| --- | --- |
| `scripts/triton/softmax2d.py` | 逐行 softmax 内核 |
| `scripts/triton/softmax2d.ttir` | canonical 黄金文件 |
| `scripts/triton/e2e/softmax2d/` | 8×8 f32 输入与 64 个逐位参考值 |
| `scripts/check-ch31.sh` | 5 项章门禁 |

代码落点：`TTIR.h`/`TTIR.cpp`（`DivF`/`Exp2`、2D `createReduce`）、
`Frontend.cpp`（`/`、`tl.exp2`、`keep_dims`）、`LLVMEmit.cpp`
（`emitReduce2D`、expand_dims identity、`fdiv`/`exp2` 发射）、
`IntrinsicsGPUMin.td` 与 `GPUMinInstr{Formats,Info}.td`（新指令与 pattern）、
`gpumin_sim.py`（`fdiv32` 与 SFU）。
