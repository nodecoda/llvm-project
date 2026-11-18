# 第 24 章：固定布局——TTIR 到 GPUMin LLVM IR

## 目标

第 23 章结束时我们有了合法的 TTIR，但它还不是一个能跑的程序。这一章接上
后半段：

> 固定布局（一个 Triton program = 一个 GPUMin block）→ 标量化 →
> `sr.args` kernel ABI → `llc` → `gpumin_sim.py`。

验收是一个真实的 masked i32 vecadd：`tensor<256xi32>` 在 32 线程的 block
上跑出正确结果。

退出条件（可执行）：

```sh
llvm/lib/Target/GPUMin/scripts/check-ch24.sh <llvm-bin-dir>
```

## 固定布局

Triton 的 layout 在本项目里被冻结成一条规则：

```text
一个 Triton program          = 一个 GPUMin block
threadsPerWarp = 8, warpsPerCTA = 4  ->  32 threads/block
sizePerThread = 1
tensor element r*32 + tid    ->  thread tid 的第 r 个元素
```

因此 `tensor<Nxi32>` 的每个线程持有 `N/32` 个标量。第 24 章要求 `N` 是 32
的倍数；否则前端直接报错：

```text
gpumin-triton: tl.arange range must be a positive multiple of 32, got 100
```

布局与第 11 章的 `simt-model.json` 一致（`warp_size=8`、
`warps_per_block=4`），不是新引入的常量。

## kernel ABI：没有形式参数

第 13 章把 GPUMin 的 kernel ABI 定成：

```text
launcher 把参数块地址写进 ur0；
kernel 用 llvm.gpumin.args() 读出它（sr.args，SYS funct 9）；
参数块是 global memory 里的 32 位字数组。
```

LLVMEmit 生成的函数因此是 `define void @add_kernel() #0`，没有任何形参。
序言按签名顺序读参数块：

```llvm
%0 = call i32 @llvm.gpumin.args()
%1 = inttoptr i32 %0 to ptr addrspace(1)
%2 = load i32, ptr addrspace(1) %1            ; args[0] = x_ptr
%3 = inttoptr i32 %2 to ptr addrspace(1)
...
```

指针参数在参数块里是 32 位地址，进入 kernel 后 `inttoptr` 成
`ptr addrspace(1)`；`i32` 参数直接使用。这与 `scripts/e2e/launch.ll` 的
手工 ABI 完全一致。

## 标量化

`LLVMEmit` 为每个 TTIR 值保存一个 `Lowered`：

```cpp
struct Lowered {
  std::vector<llvm::Value *> Elts;  // scalar: 1, tensor<N>: N/32
  bool IsTensor;
};
```

逐 op 展开：

| TTIR | LLVM |
| --- | --- |
| `arith.constant` | `i32` 常量 |
| `tt.get_program_id x` | `llvm.gpumin.blockid()` |
| `tt.make_range` | `r*32 + tid`，每个 r 一个 `add` |
| `tt.splat` | 同一个标量值复制到 `N/32` 个元素 |
| `tt.addptr` | 每个元素一个 `getelementptr i32` |
| `tt.load` | 每个元素一个 `load i32, ptr addrspace(1)` |
| `tt.store` | 每个元素一个 `store` |
| `arith.*` / `arith.cmpi` | 逐元素标量指令 |

`tid` 由 `llvm.gpumin.tid()` 提供；`r*32 + tid` 与布局规则是同一个公式。

## mask：分支 + phi

GPUMin 的 load/store 没有谓词操作数（第 06 章的 `ld/st` 是普通访存）。
masked load 必须显式控制流：

```llvm
  br i1 %mask, label %load, label %merge
load:
  %v = load i32, ptr addrspace(1) %p
  br label %merge
merge:
  %x = phi i32 [ %v, %load ], [ 0, %before ]
```

masked store 同理，用分支包住 `store`。这正好复用第 05/14 章的分歧与重汇聚
机制：mask 为假的 lane 在分支处失活，merge 后恢复。

## 结果

`BLOCK=256`、`grid=1`、`block=32` 的 launch：

```text
x[i] = i           (i = 0..255)
y[i] = 1000 + i
out[i] = x[i] + y[i]
```

模拟器输出：

```text
1000 1002 1004 1006 1008 1010 1012 1014
```

门禁还会检查：

- IR 里没有 `nvvm`/`ptx` 残留；
- 汇编里有 `sr.args`、`sr.tid`、`sr.blockid`，并以 `exit` 结束；
- `tensor<256xi32>` 正好展开成 8 个 store。

## 已知限制：BLOCK 上限

本章的标量化是**急切展开**：TTIR 里所有 x 元素先 load，再所有 y 元素
load，最后才相加。`BLOCK=256` 时每个线程 8 个元素，寄存器压力刚好可承受；
`BLOCK=320` 以上会触发寄存器分配溢出，而当前 GPUMin 后端对**谓词寄存器
的栈槽**支持不完整（`PRED` 的 spill 走 32 位 local store），会在 regalloc
里断言失败。

这不是 TTIR 或布局的问题，而是后端第 07 章栈支持的缺口。可选修复方向：

1. 给谓词寄存器定义合法的 spill 槽与存取指令；
2. 在 LLVMEmit 里按元素做局部调度，降低同时活跃的 mask/value 数量。

本章选择先把 `BLOCK=256` 跑通，把 1024 留作后续优化/后端修复的练习。

## 实现文件

| 文件 | 作用 |
| --- | --- |
| `LLVMEmit.{h,cpp}` | TTIR → LLVM IR；布局、标量化、mask、参数块 |
| `gpumin-triton.cpp` | `--emit-llvm` |
| `scripts/check-ch24.sh` | 本章门禁（IR 形状、汇编、模拟器结果） |

## 下一章

第 25 章补 f32：

- TTIR 的 `arith.addf/mulf/cmpf`；
- GPUMin 的 `fadd/fmul/fsetp` 指令与寄存器约束；
- mask 与 f32 访存；
- 用同一个 vecadd 形状跑浮点结果。
