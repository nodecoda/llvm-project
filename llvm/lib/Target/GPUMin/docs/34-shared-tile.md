# 第 34 章：shared tile 与列/行切片

## 目标

ch30 的 matmul 每个 block 算一个 8×8 输出 tile，但 K 循环里每一列 A、
每一行 B 都是从 global 重新读的。问题是**布局**：`[8,1]` 的 tile 按
“warpid 选行组、laneid 选列”摊到 32 个线程上，4 个 warp 会各自把同一列
A 读一遍（每线程 2 个元素）。一个 K 步就有 2 次重复读，32 步就是 64 次。

本章给子集加上显式的 shared staging：

```python
@triton.jit
def matmul_shared_kernel(a_ptr, b_ptr, c_ptr):
    pm = tl.program_id(0)
    pn = tl.program_id(1)
    rm = pm * 8 + tl.arange(0, 8)[:, None]
    rn = pn * 8 + tl.arange(0, 8)[None, :]
    kk_r = tl.arange(0, 32)[None, :]
    kk_c = tl.arange(0, 32)[:, None]

    sa = tl.alloc_shared((8, 32), tl.float32)
    sb = tl.alloc_shared((32, 8), tl.float32)

    a_tile = tl.load(a_ptr + rm * 32 + kk_r)
    tl.store_shared(sa, a_tile)
    b_tile = tl.load(b_ptr + kk_c * 32 + rn)
    tl.store_shared(sb, b_tile)

    acc = tl.zeros((8, 8), tl.float32)
    for k in tl.static_range(0, 32):
        acc = acc + tl.load_shared_col(sa, k) * tl.load_shared_row(sb, k)
    tl.store(c_ptr + rm * 32 + rn, acc)
```

`[8,32]` 和 `[32,8]` 都正好是每线程 8 个元素——ch24/ch29 的固定布局上限
（`LLVMEmit.cpp:36`）。launch descriptor 和参考值与 ch30 完全相同，所以
“结果逐位一致”不是另造数据，而是直接复用 `e2e/matmul2d/`。

退出条件（可执行）：

```sh
llvm/lib/Target/GPUMin/scripts/check-ch34.sh <llvm-bin-dir>
TRITON_OPT=<triton-opt> \
  llvm/lib/Target/GPUMin/scripts/triton-verify-all.sh <llvm-bin-dir>
```

`check-ch34.sh` 覆盖 5 项：TTIR + canonical 对拍、共享指令与屏障顺序、
1024 个输出逐位一致、`glb_ld` 至少减半、以及四条作用域诊断。

## 为什么是 shared，而不是再来一次 global

shared 与 global 的差别不在“快”，而在**可见性**：shared 是 block 级
的，一个线程写进去，其它 warp 也能按任意行列读出来。于是“计算出来的
tile 不能列切片”（ch32 的边界）在这里自然消失——我们不再对寄存器 tile
切片，而是对一个 block 共享的数组做布局无关的读取。

| 访问 | ch30 | ch34 |
| --- | --- | --- |
| A 的一列 | 4 个 warp 各读 8 行 | 1 次 staging，之后读 shared |
| B 的一行 | 每个 warp 都读整行 | 1 次 staging，之后读 shared |
| 每 block global 元素读 | 32 步 × (8+8) × 4 重复 | 8×32 + 32×8 = 512 |

## TTIR：三个未注册 op

shared 句柄没有自己的类型：文本上仍然是 `tensor<8x32xf32>`，因为 Triton
的 `!tt.ptr` 地址空间只有 global/descriptor/constant，没有 shared
（第四课规划里的探针已经证实）。真正区分“句柄”和“值 tile”的是前端：

```mlir
%sa = "gpumin.shared_alloc"() : () -> tensor<8x32xf32>
"gpumin.shared_store"(%sa, %a_tile) : (tensor<8x32xf32>, tensor<8x32xf32>) -> ()
%c = "gpumin.shared_col"(%sa) <{index = 3 : i32}> : (tensor<8x32xf32>) -> tensor<8x1xf32>
%r = "gpumin.shared_row"(%sb) <{index = 3 : i32}> : (tensor<32x8xf32>) -> tensor<1x8xf32>
```

`index` 是**属性**，不是操作数：它来自 `tl.static_range` 的循环变量，
编译期必须是常量，因此可以在前端做越界检查。动态索引留给后续章节。

这是全课程唯一需要 `--allow-unregistered-dialect` 的地方：

```text
$ triton-opt matmul_shared.ttir
error: operation being parsed with an unregistered dialect ...
$ triton-opt --allow-unregistered-dialect --canonicalize matmul_shared.ttir
（通过，gpumin.* 原样保留）
```

所以 gate 的第 1 项两侧都带这个 flag，golden 对拍依旧稳定。

## 降级：AS2 全局 + 自动屏障

- `shared_alloc` → 一个 `addrspace(2)` 的 `GlobalVariable`，internal、
  align 4。偏移由 ch16 的 `assignSharedOffsets` 按声明顺序分配
  （`GPUMinISelDAGToDAG.cpp:99-108`），降成 `sr.shared + off`，
  **不需要任何新指令**。
- `shared_store` → 逐元素 `st.shared.b32`，地址 = `row*C + col`，
  与 ch31 归约的写回循环同构（`LLVMEmit.cpp:337-345`）。
- `shared_col/row` → 每线程读 `Rows`（列）或 `Cols`（行）个元素，
  行列坐标来自同一个 `coordValue`。
- 写后读的屏障由**后端自动插入**：第一个 slice 读之前如果发生过 store，
  先发一条 `llvm.gpumin.barrier()`。两个 staging store 只产生一条
  `bar.sync`——用户在 kernel 里不需要手写 barrier。

## 诚实记录代价

同一 launch 下（4×4 blocks、32³）比较两个内核：

```text
rank-1: glb_ld=50688 shm_ld=0     loc_st=25088 cycles=16608 barriers=0
shared: glb_ld=9728  shm_ld=49152 loc_st=70656 cycles=41200 barriers=16
```

`glb_ld` 下降 **5.2×**，这正是本章要证明的收益。但 cycles 反而变差：
K 循环在 TTIR 里是完全展开的，96 次 slice 读对应 96 个地址表达式，
机器调度把它们提到屏障之前，寄存器分配只能把它们溢出到 local。
`loc_st` 从 25088 涨到 70656 就是这笔账。

这不是实现错误，而是**教学子集的真实取舍**：展开换来了确定的布局和
可读的 TTIR，代价是地址压力。生产编译器会让 K 循环保持 roll，或对地址
做重物化；ch37 的 benchmark 会把这两个数字一起锁进基线，提醒读者
“少读 global”并不自动等于“更快”。

## e2e 数据

`e2e/matmul_shared/` 直接复用 `e2e/matmul2d/` 的 `launch.json` 与
`expected.json`（只把 kernel 名改成 `matmul_shared_kernel`）。
`check-ch34.sh` 第 3 项还额外断言两份 `values` 完全相同，防止有人悄悄
改参考值。

## 诊断

四条新错误都指向具体写法：

```text
tl.store_shared source shape does not match the shared tile
'k' is not a compile-time integer
the shared column index is out of range
a shared tile handle cannot be used in arithmetic; read it with tl.load_shared_col/row
```

最后一条是必要的：句柄在文本上是 tensor，类型系统分不出来，前端用
`Value::isShared()` 显式拦截，避免把 `%sa` 当值 tile 送进 LLVMEmit。

## 下一步

ch35 的 `tl.dot` 不引入 MMA，也不新增 ISA：它把 8×8 的 A/B 走本章的
alloc/store/col/row，再复用同一个 K 步 rank-1 累加。本章的 e2e 就是它的
回归基线。
