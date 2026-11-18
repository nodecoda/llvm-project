# 第 32 章：注意力 capstone（第三课收束）

## 目标

第三课的最后一章不再新增 ISA。它把 ch28–ch31 的零件拼成一个完整的
GPU kernel：8×8 单头 scaled dot-product attention。

```python
@triton.jit
def attn_kernel(q_ptr, k_ptr, v_ptr, p_ptr, o_ptr):
    r = tl.arange(0, 8)[:, None]          # [8,1]
    c = tl.arange(0, 8)[None, :]          # [1,8]
    acc = tl.zeros((8, 8), tl.float32)
    for t in tl.static_range(0, 8):       # S = Q K^T
        acc = acc + tl.load(q_ptr + r * 8 + t) * tl.load(k_ptr + c * 8 + t)
    m = tl.max(acc, axis=1, keep_dims=True)
    p = tl.exp2((acc - m) * 1.4426950408889634)
    s = tl.sum(p, axis=1, keep_dims=True)
    p = p / s
    tl.store(p_ptr + r * 8 + c, p)
    tl.debug_barrier()
    out = tl.zeros((8, 8), tl.float32)
    for t in tl.static_range(0, 8):       # O = P V
        out = out + tl.load(p_ptr + r * 8 + t) * tl.load(v_ptr + t * 8 + c)
    tl.store(o_ptr + r * 8 + c, out)
```

退出条件（可执行）：

```sh
llvm/lib/Target/GPUMin/scripts/check-ch32.sh <llvm-bin-dir>
TRITON_OPT=<triton-opt> \
  llvm/lib/Target/GPUMin/scripts/triton-verify-all.sh <llvm-bin-dir>
```

`check-ch32.sh` 覆盖 5 项：TTIR/canonical 对拍、ISA 面不变、屏障前后
的访存顺序、64 个输出的逐位比较、以及新接口的诊断。

## 三块拼图

| 阶段 | 用到的章节 | 在 TTIR 里的样子 |
| --- | --- | --- |
| `S = Q K^T` | ch29 2D/broadcast、ch30 rank-1 update | 8 组 `mulf` + `addf`，`[8,1] × [1,8] → [8,8]` |
| softmax | ch31 2D 归约、SFU、`fdiv` | 两个 `tt.reduce axis=1` + `math.exp2` + `arith.divf` |
| `O = P V` | ch30 rank-1 update（P 从全局重读） | 8 组 `mulf` + `addf` |

整个 kernel 只有 321 行汇编，但把 2D tile、广播、静态展开、shared
归约、超越函数和块内屏障全部串了起来。

## 为什么 P 要绕一次全局内存

softmax 的 P 是**计算出来的** `[8,8]` 寄存器 tile。子集里 `[.., None]`
只能作用于 `tl.arange`（ch29 的边界），没有“取计算结果第 t 列”的能力：

```text
chapter 29 only supports [:, None] and [None, :]
```

所以第二次 matmul 需要 `P[:, t]` 时，P 先写回全局 scratch，再按列读回：

```text
store p -> bar.sync -> load p
```

`p_ptr` 是 launch 里传入的 64 个 f32 scratch。它和 `o_ptr` 分开，是为了让
`P` 在最后仍然可观察；生产实现会把这块数据留在 shared memory 里，这也
是下一课的第一项改进。

## `tl.debug_barrier()`：本章唯一的新接口

P 的行由不同 warp 持有。写回全局后，如果不加屏障，模拟器里可能发生：

```text
warp 0: store P 行 0/1 -> 直接进入第二个循环 -> 读到 warp 1 还没写的行
```

所以 `tl.debug_barrier()` 是语义必需，而不是调试装饰。Triton 3.9 自己的
TTIR 把它打印成：

```mlir
ttg.barrier all
```

后端复用 ch17 的屏障 intrinsic，降成同一条 `bar.sync`：

```mlir
call void @llvm.gpumin.barrier()
```

汇编里可以数到 5 条 `bar.sync`：两次归约各 2 条，加上这条 P/V 屏障。
`check-ch32.sh` 的第 3 项正是用“最后一条 `bar.sync` 之前必须有
`st.global`、之后必须有 `ld.global`”来锁住这个顺序。

## e2e 数据

Q/K/V 都是 1/8 的倍数或小整数，所以 `Q K^T` 全部精确：

```text
Q 行 i: start = -0.75 + 0.125*i, step = 0.25
K 行 i: start =  0.50 - 0.125*i, step = -0.25
V 行 i: start =  1 + i,         step = 1
```

`S` 落在约 ±3 的范围内，`exp2` 不会整体下溢；每行 attention 权重和都是
`1.0`。参考实现按模拟器的单精度语义逐步舍入（`f32` 加减乘除、
`f32(2^t)`、从左到右累加），64 个输出与模拟器逐位相等。

## 统计

模拟器（1 个 block、32 线程）：

```text
issued=1236 lane_instrs=9888 cycles=938 barriers=5
alu=836 lsu=380 bar=20 glb_ld=1696 shm_ld=1024 shm_st=128
```

| 指标 | 数值 | 说明 |
| --- | --- | --- |
| 汇编行数 | 321 | 两次 8 步展开 |
| `fadd`/`fmul` | 46 / 34 | 16 外积 + 16 次 P·V + 归约 |
| `fsetp`/`selp` | 14 / 14 | 两次 max 归约的比较 |
| `ld.local`/`st.local` | 1 / 1 | 只有 callee-saved 的保存/恢复，没有溢出 |
| `glb_ld` | 1696 | Q/K/V 与 P 的重复读；shared tiling 是下一课的题目 |

对比 ch30 的 matmul（98 条 local spill），这个 kernel 的寄存器压力小得多：
每个循环只有 8 步，而且 P·V 的第二个累加器可以复用第一个的生命周期。

## 第三课收束

| 章 | 主题 | 留下的能力 |
| --- | --- | --- |
| 28 | 元素级完整性 | `//`/`%`/`tl.cdiv`、min/max、广播 |
| 29 | 2D tensor 与 2D grid | 8×8 tile、4×8 线程、`expand_dims`/`broadcast`/`trans` |
| 30 | 分块 matmul | rank-1 update、静态展开、寄存器压力 |
| 31 | 2D 归约与 softmax | `keep_dims`、shared 归约、`exp2`、`fdiv` |
| 32 | 注意力 capstone | 三块拼图 + `tl.debug_barrier()`，ISA 零新增 |

仍然不做的（有意留白）：

| 主题 | 原因 |
| --- | --- |
| `tl.dot` | 需要 MMA 语义或 shared tile 的列切片；roadmap 明确列为非目标 |
| shared memory 上的 P·V | 需要“计算 tile 写 shared + 列切片”两个新能力 |
| mask/因果注意力 | `tl.where` 已在 ch27，但 `tl.load(mask=...)` 在 2D tile 上还没有入口 |
| 多 block / 多 CTA | 当前 attention 只用 1 个 block；grid 已支持 2D |
| async copy / TMA / warp specialization | 属于真实 GPU 的存储层次与调度特性 |
| 性能优化 | 本课只保证语义正确、IR 可读、门禁可复现 |

## 本章新增文件

| 文件 | 作用 |
| --- | --- |
| `scripts/triton/attention.py` | capstone 内核 |
| `scripts/triton/attention.ttir` | canonical 黄金文件 |
| `scripts/triton/e2e/attention/` | 5 个参数、8×8 Q/K/V、64 个逐位参考值 |
| `scripts/check-ch32.sh` | 5 项章门禁 |
| `docs/32-attention.md` | 本章 |

代码落点：`TTIR.h`/`TTIR.cpp`（`Barrier`、`ttg.barrier all`）、
`Frontend.cpp`（`tl.debug_barrier()`）、`LLVMEmit.cpp`
（Barrier → `llvm.gpumin.barrier`）。
