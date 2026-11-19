# 第 6 章：reduce、shared memory 与 barrier

前五章的块级值都是“每个线程持有若干个元素”。这一章第一次把所有线程的
局部结果合并成一个值：

```python
offs = tl.arange(0, 1024)
x = tl.load(x_ptr + offs)
total = tl.sum(x, axis=0)
tl.store(out_ptr, total)
```

TTIR 里它是一条 `tinytt.reduce`：

```mlir
%4 = tinytt.reduce %3 <axis = 0, combine = "add"> : tensor<1024xf32> -> f32
```

注意结果类型是 `f32`，不是 `tensor<...>`。这是本章最重要的类型变化：
reduce 把 block 级 tensor 折叠成线程级标量。

## 1. 为什么 ODS 结果不能写 `AnyRankedTensor`

第一版把 `TinyCommon_Reduce` 的结果写成 `AnyRankedTensor`，前端却用
`f32` 构造 `ReduceOp`。TableGen 生成的 builder 会把结果强转成
`TypedValue<RankedTensorType>`，于是在 verifier 有机会报错之前就触发：

```text
cast<TypedValue<RankedTensorType>>() argument of incompatible type
```

正确做法是：

```tablegen
let results = (outs AnyType:$result);
```

语义约束交给 `verifyReduce`：

- 输入必须是 1-D ranked tensor；
- `axis` 必须是 0；
- 结果不能是 tensor；
- 结果元素类型必须等于输入元素类型；
- `combine` 必须是 `add` / `max` / `min`。

这样即使写错，也会得到可读的 verifier 诊断，而不是崩溃。

## 2. 线程内先 reduce，再跨线程

1024 元素、32 线程，每个线程持有 32 个元素。直接让每个线程把 32 个值
都写 shared，会用掉 1024 个 slot；本章先在寄存器里做 32 次加法：

```text
partial = x[0] + x[1] + ... + x[31]
```

然后每个线程只写一个 partial：

```text
shared[tid] = partial
bar.sync
sum = shared[0] + shared[1] + ... + shared[31]
```

`llvm.gpumin.shared` 返回本 block 的 shared window 基址，地址空间是 2。
本章从 `sharedOffset` 开始分配 32 个 4 字节 slot，写完后把
`sharedOffset` 增加 `4 * threads`，保证下一个 reduce 不会覆盖这一段。

## 3. 为什么读回的是 `threads`，不是 `elementsPerThread`

这两个数只在本章的 1024 元素例子里恰好都等于 32：

| 量 | 1024 元素 | 512 元素 |
| --- | --- | --- |
| 每线程元素数 | 32 | 16 |
| block 线程数 | 32 | 32 |
| shared partial 数 | 32 | 32 |

如果读回循环跑 `elementsPerThread` 次，512 元素只会读 16 个 partial，
结果是 256 而不是 512。实现必须从布局计算线程数：

```text
threads = product(threadsPerWarp[d] * warpsPerCTA[d])
```

`check-t6.sh` 同时跑 1024 和 512 两个例子，后者就是专门抓这个错误的。

## 4. barrier 的位置

本章的 shared 访问顺序是：

```text
write shared[tid]
bar.sync
read shared[0..threads)
```

只需要一个 barrier：

- barrier 之前，每个线程写自己的 slot；
- barrier 之后，所有线程读所有 slot；
- 读阶段没有新的 shared 写，因此不需要第二个 barrier。

如果后续章节复用同一段 shared，必须在“读完之后、下一次写之前”再插
一个 barrier。本章用单调增长的 `sharedOffset`，不复用，所以不需要。

## 5. 标量 store 只由 lane 0 执行

reduce 之后所有线程都持有相同的 `sum`。如果每个线程都执行
`store_scalar`，值虽然相同，但内存语义上是一次冗余写。本章用
`tid == 0` 分支保护：

```mlir
%467 = llvm.icmp "eq" %0, %466 : i32
llvm.cond_br %467, ^bb1, ^bb2
^bb1:
  llvm.store %465, %12 : f32, !llvm.ptr<1>
  llvm.br ^bb2
^bb2:
  llvm.return
```

这也让 LLIR 的 CFG 与真实 kernel 的“一个线程写结果”一致。

## 6. 指令映射

| TTGIR | LLVM dialect / GPUMin |
| --- | --- |
| `tinyttg.reduce <add>` | 线程内 `fadd` → `st.shared` → `bar.sync` → 32 次 `ld.shared` + `fadd` |
| `tinyttg.store_scalar` | `icmp eq tid, 0` → `cond_br` → `llvm.store` |
| shared 指针 | `!llvm.ptr<2>` / `sr.shared` |
| barrier | `llvm.gpumin.barrier` / `bar.sync` |

## 7. golden 与门禁

- `kernels/reduce_sum.ttir`：前端产物；
- `tests/passes/reduce_sum.ttgir`：T4 产物；
- `tests/passes/reduce_sum.llir`：T5 产物；
- `tests/negative/reduce-tensor-result.ttir`：结果必须是标量；
- `tests/negative/reduce-bad-combine.ttir`：combine 必须合法；
- `tests/negative/store-scalar-tensor.ttir`：store_scalar 的 value 必须是标量；
- `tests/frontend/reduce512.py`：16 元素/线程的 512 元素回归。

`check-t6.sh` 五项：

1. 前端产出 `tinytt.reduce ... -> f32` 与 `store_scalar`，且 `--verify-each` 接受；
2. T4/T5 输出与 golden 逐字节一致，LLIR 含 `llvm.gpumin.barrier`、`!llvm.ptr<2>`、`llvm.cond_br`；
3. `mlir-translate` + `llc -march=gpumin -verify-machineinstrs` 成功，汇编含 `bar.sync`、`sr.shared`、`st.shared`、`ld.shared`；
4. 1024 与 512 元素都得到正确的 block sum（`1024.0f` / `512.0f`）；
5. 三个负例都被 verifier 拒绝，且诊断指向正确的规则。

## 8. 已知边界（第 7 章补齐）

- 只支持 1-D、axis 0；
- lowering 只支持 `combine = "add"`；`max/min` 已在 verifier 中保留；
- 不做 2-D reduce；
- 不做 mask；
- shared 只按 `sharedOffset` 单调分配，不做复用/压缩。

## 9. 退出条件

- `ninja -C <build> tiny-opt tiny-triton` 成功；
- `check-t6.sh` 5/5；
- `check-t1.sh`–`check-t5.sh` 全绿；
- `gpumin-verify-all.sh` 8/8；
- `triton-verify-all.sh` 0 failed；
- `git tag tiny-t06` 指向本章 commit。
