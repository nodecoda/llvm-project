# 第 26 章：reduction——shared 内存与双 barrier

## 目标

`tl.sum` / `tl.max` 是 Triton 里第一个**跨 lane** 的语义：结果不在任何单个
线程的寄存器里，而是整个 block 的 32 个线程一起算出来的。本章用第 16/17
章已经验证过的 shared 内存与 barrier 实现它：

1. 每个线程先归约自己持有的元素，得到一个 partial；
2. 32 个 partial 写进 block 的 shared 窗口；
3. 第一次 `bar.sync` 后，tid 0 串行合并 32 个值；
4. 结果写回 `window[0]`，第二次 `bar.sync` 后所有线程读取；
5. TTIR 里这是一条真正的 `tt.reduce` 区域，`triton-opt` 可以解析。

退出条件（可执行）：

```sh
llvm/lib/Target/GPUMin/scripts/check-ch26.sh <llvm-bin-dir>
```

## TTIR：tt.reduce 是一个带 region 的 op

Triton 的 reduction 不是一条 flat 指令，而是一个 region：

```mlir
%total = "tt.reduce"(%x) <{axis = 0 : i32}> ({
  ^bb0(%a: i32, %b: i32):
    %v = arith.addi %a, %b : i32
    tt.reduce.return %v : i32
}) : (tensor<32xi32>) -> i32
```

区域里的合并指令就是“如何把两个标量合成一个”。本章只发射两种：

| Python | 区域内的合并 | 结果类型 |
| --- | --- | --- |
| `tl.sum(x, axis=0)` | `arith.addi` / `arith.addf` | `i32` / `f32` |
| `tl.max(x, axis=0)` | `arith.maxsi` | `i32` |

`tt.reduce.return` 把合并结果交回外层。前端只接受 1D tensor 和 `axis=0`；
其它形状在 `Frontend.cpp` 报错，不会悄悄算错。

## 布局：每线程一个 partial

第 24 章的固定布局仍然成立：`tensor<BLOCKxT>` 的每个线程持有
`BLOCK/32` 个元素。reduction 先在线程内部完成：

```text
BLOCK=32:  每线程 1 个元素   -> partial = 该元素
BLOCK=64:  每线程 2 个元素   -> partial = e0 op e1
```

这样 shared 往返永远只有 32 个字，和 BLOCK 无关。也正因为如此，
`BLOCK=64` 的求和 1..64 仍然是 2080。

## LLVM IR：一个 addrspace(2) 窗口

每条 reduction 在模块里分配一个 block 级窗口：

```llvm
@__gpumin_red_reduce_sum_0 =
  internal addrspace(2) global [32 x i32] zeroinitializer, align 4
```

发射顺序（`LLVMEmit.cpp: emitReduce`）：

```llvm
%partial = ...                       ; 线程内归约
%slot = getelementptr [32 x i32], ptr addrspace(2) @window, i32 0, i32 %tid
store i32 %partial, ptr addrspace(2) %slot
call void @llvm.gpumin.barrier()     ; barrier 1
%first = icmp eq i32 %tid, 0
br i1 %first, label %red.sum, label %red.merge
red.sum:
  ; 串行读 window[0..31] 并合并
  store i32 %acc, ptr addrspace(2) @window
  br label %red.merge
red.merge:
  call void @llvm.gpumin.barrier()   ; barrier 2
  %result = load i32, ptr addrspace(2) @window
```

### 为什么需要第二个 barrier

第一个 barrier 保证“所有 partial 都写完了”，tid 0 才能开始合并。
但 tid 0 的合并结果写回 `window[0]` 之后，其它线程还不知道。第二个
barrier 把这次写变成 block 内可见，随后所有线程都能 `load` 到同一个标量。
这样 `tl.sum` 的返回值在语义上就是普通的 SSA 值，可以继续参与后续运算。

两个 barrier 都在**统一控制流**上，不在 `if` 里面；sim 的分歧模型因此
不需要在分支内部处理 barrier。

## 验收数字

单 block（32 线程）：

| 输入 | `tl.sum` | `tl.max` |
| --- | --- | --- |
| 32 个 1 | 32 | 1 |
| 1..32 | 528 | 32 |
| 1..64（BLOCK=64） | 2080 | 64 |

两个 block 各自读 32 个字：第一个 block 全 1，第二个全 2，输出必须是
`32 64`。这证明 shared 窗口是 **block 作用域**，不是 grid 作用域。

`--stats` 的对应证据：

```text
barriers=4 shm_ld=128 shm_st=66
```

（两个 block × 两个 barrier = 4；每个 block 32 次 shared 写 + 32 次读。）

## f32 求和与 i32 最大值

- `tl.sum` 对 f32 走 `arith.addf` + `fadd`，shared 槽是 `[32 x float]`；
  32 个 1.0 求和得到 32.0（位模式 `0x42000000`）。
- `tl.max` 目前只支持 i32：合并用 `setp.gt` + `selp`。
  f32 的 max 需要 `select` 一个浮点值，而 GM32 还没有 f32 select 的
  合法化；它会在第 28 章和元素级运算一起补上。前端会明确报
  “arrives in chapter 28”，而不是选错指令。

## 已知边界

- 只支持 `axis=0` 和 1D tensor；`tl.sum(x, axis=1)` 报错。
- tid 0 串行合并 32 个值，复杂度 O(32)；树形归约留作优化，不改变语义。
- 每条 reduction 静态分配一个 32 槽窗口；多个 reduction 会各占一个偏移。
- warp 内没有 shuffle，跨 warp 也没有；shared 是唯一通道。

## 验收输出

```text
PASS 1/5: TTIR contains a tt.reduce region and a scalar store
PASS 2/5: triton-opt parses the reduction and it matches the golden
PASS 3/5: the reduction uses shared memory and two bar.sync
PASS 4/5: one block gives 32/528 and two blocks stay independent
PASS 5/5: tl.max, f32 tl.sum and the shared/barrier stats agree
```
