# 第 20 章：matmul 验收——两种实现的数字

## 目标

本章不引入新指令，也不改后端。它做一件事：**用前面所有章的东西，解释
两个真实实现的差别**。

1. 一个 4×4 矩阵乘（`C = A × B`），16 条 lane、2 个 warp；
2. 两个版本：`matmul_naive`（每条 lane 直接读 A 的行和 B 的列）和
   `matmul_tiled`（先把 A、B 搬进共享窗口，barrier 后再算）；
3. 用第 18/19 章的模型回答：**tiling 到底划算吗**；
4. 答案是"看模型"：延迟为 0 时 tiling 更慢，延迟真实时 tiling 反超。

退出条件（可执行）：

```sh
llvm/lib/Target/GPUMin/scripts/check-ch20.sh <llvm-bin-dir>
```

## 背景

### 为什么用 matmul 验收

因为它同时压到四件不同的事：

| 机制 | 在哪出现 |
| --- | --- |
| 调用约定与栈 | `@mul` 是 device function，两个 kernel 都调用它 |
| 循环与分歧 | `@mul` 的 shift-add 循环 trip count 依赖数据 |
| 共享内存 | tiled 版本把 A/B 放进 `addrspace(2)` |
| barrier | tiled 版本必须在两阶段之间同步 |

一个能跑对 4×4 matmul 的 target，上面这些都至少被验证过一次。这就是它
比"再写一个向量加法"更适合做验收的原因。

### 这个 ISA 没有乘法指令

第 15 章的 `saxpy` 已经暴露过：GPUMin 的 ALU 只有
`add/sub/and/or/xor/shl/shr/sar`，没有 `mul`。当时的做法是把常数乘法变成
移位加法。

矩阵乘没法这么消，因为两个操作数都是变量。所以本章写一个 **shift-add
乘法器**：

```llvm
loop:
  %x = phi i32 [ %a, %entry ], [ %x2, %body ]
  %y = phi i32 [ %b, %entry ], [ %y2, %body ]
  %acc = phi i32 [ 0, %entry ], [ %acc2, %body ]
  %done = icmp eq i32 %y, 0
  br i1 %done, label %exit, label %body
body:
  %bit = and i32 %y, 1
  %nz = icmp ne i32 %bit, 0
  %term = select i1 %nz, i32 %x, i32 0
  %acc2 = add i32 %acc, %term
  %x2 = shl i32 %x, 1
  %y2 = lshr i32 %y, 1
  br label %loop
```

它同时是 `phi`、`select`（第 14 章的 `SELP` 路径）、循环和调用的联合测试。

### tiling 换的是什么

4×4、16 条 lane，每条 lane 负责一个输出元素：

```text
naive:  lane(t) 读 A 的第 r 行(4 个) + B 的第 c 列(4 个) = 8 次 global load
        全 block: 8 × 16 = 128 次

tiled:  阶段 1: 每条 lane 搬 1 个 A 元素 + 1 个 B 元素 = 2 次 global load
        阶段 2: 每条 lane 从共享内存读 4 + 4 = 8 次 shared load
        全 block: 32 次 global + 128 次 shared
```

**global 少了 4 倍，但 load 总次数没少，还多了 2 次 store 和 1 次 barrier。**
所以"tiling 更快"不是免费的——它把"慢的 global 访问"换成"快的 shared 访问"，
代价是额外指令。是否划算，取决于两者的延迟差。

## 设计

### 数据布局

A、B、C 都是 4×4、行主序，各占 16 个字：

```text
A: 0x000 .. 0x03f     A[i][j] = A_base + 4*(4i + j)
B: 0x800 .. 0x83f
C: 0x1000 .. 0x103f
```

lane `t` 负责 `C[r][c]`，其中 `r = t >> 2`、`c = t & 3`。16 条 lane 正好
填满 2 个 warp。

### 共享窗口布局

```text
tile[0..15]   = A
tile[16..31]  = B
```

每条 lane 搬一个元素：`tile[t] = A[t]`、`tile[16 + t] = B[t]`。16 条 lane
16 个元素，没有分支、没有循环，一个 warp 一条 `st.shared` 就搬完了。

### 为什么必须 barrier

阶段 2 里，lane 0 读的是 `tile[0..3]`（warp 0 搬的）和 `tile[16..19]`
（warp 0 搬的）；lane 12 读的是 `tile[12..15]`（warp 1 搬的）。跨 warp 的
读，**没有 barrier 就是第 17 章那个竞态**。

`check-ch20.sh` 断言 `barriers=1`，`bar=2`（每个 warp 一条 `bar.sync`）：
release 次数和指令次数不是一回事，这两个数字一起看才说明"两个 warp 都到了、
放行了一次"。

## 实现

两个 kernel 除了取操作数的地方，其余完全相同：

```llvm
; naive
%va0 = load i32, ptr addrspace(1) %pa       ; A[r][0]
...
%vb0 = load i32, ptr addrspace(1) %pb       ; B[0][c]
...
%m0  = call i32 @mul(i32 %va0, i32 %vb0)

; tiled
%va  = load i32, ptr addrspace(1) %pa       ; A[t]
%vb  = load i32, ptr addrspace(1) %pb       ; B[t]
store i32 %va, ptr addrspace(2) %sta
store i32 %vb, ptr addrspace(2) %stb
call void @llvm.gpumin.barrier()
%qa0 = load i32, ptr addrspace(2) %pa4      ; tile[r*4 + 0]
...
%qb0 = load i32, ptr addrspace(2) %pb4      ; tile[16 + 0*4 + c]
...
%m0  = call i32 @mul(i32 %qa0, i32 %qb0)
```

`@mul` 的循环在 `%y == 0` 时提前结束，所以它的开销**随操作数变化**。这不是
细节：见"常见坑"第 1 条。

## 验证

四组门禁：

| 组 | 断言 | 说明 |
| --- | --- | --- |
| 1 | 两个 kernel 的输出都等于参考乘积 | 正确性 |
| 2 | `glb_ld` 128 → 32，`shm_ld` 0 → 128 | tiling 换到了什么 |
| 3 | 延迟 0：tiled 更慢（685 vs 647） | 额外指令的代价 |
| 4 | `global=32,shared=2`：tiled 更快（758 vs 886） | 换来的收益 |

### 完整数字

输入：`A[i][j] = 1 + 4i + j`，`B[i][j] = 17 + 4i + j`。

参考输出：

```text
250 260 270 280   618 644 670 696   986 1028 1070 1112   1354 1412 1470 1528
```

两个 kernel 都逐字匹配。统计：

| | naive | tiled |
| --- | --- | --- |
| `issued` | 670 | 716 |
| `alu` / `lsu` / `bar` | 612 / 58 / 0 | 648 / 66 / 2 |
| `glb_ld` | 128 | 32 |
| `shm_ld` | 0 | 128 |
| `loc_ld` | 160 | 160 |
| `barriers` | 0 | 1 |
| `cycles`（latency 0） | 647 | 685 |
| `cycles`（32 / 2） | 886 | 758 |
| `cycles`（200 / 25） | 2230 | 1278 |

三行 `cycles` 就是本章的全部结论：

```text
latency 0            naive 快 6%      tiling 白花 46 条指令
latency 32 / 2       tiled 快 15%     延迟开始显形
latency 200 / 25     tiled 快 74%     接近真实 GPU 的量级
```

第 3 组门禁锁的是第一行，第 4 组锁第二行。第一行看起来"反直觉"，但它才是
诚实的：**没有延迟，共享内存就没有价值。**

## 常见坑

### 1. 乘法器的抽象层次会改结论

第一版 `@mul` 固定跑 32 步。结果：

```text
naive: issued=3502   cycles=3479
tiled: issued=3548   cycles=3517
latency 8：两者都只多几十拍
```

原因很直接：每条 lane 要做 4 次乘法 × 32 步 ≈ 2600 条指令，而整个内存
部分只有 16 次 load。**内存占比 < 1%，tiling 优化的是 1% 里的 4 倍。**

换成提前退出的循环（操作数 17–32，6 位，实际跑 6 步左右）后，
`issued` 从 3502 掉到 670，内存占比升到 ~20%，tiling 的收益才浮出来。

教训不是"要写快代码"，而是：**测量之前先确认被测的东西占多大比重。**
一个 32 步的软件乘法会把任何缓存优化都淹掉。

### 2. 软件乘法让时间是数据相关的

真实 GPU 的整数乘法是常数时间。`@mul` 的循环步数取决于 `%b` 的二进制
长度，所以本章的 `cycles` **随输入数据变化**：

```text
B = 17..32（5-6 位）    -> 每 lane ~6 步
B = 1..16（1-5 位）     -> 每 lane ~4 步
```

门禁因此固定输入（`--fill 0x800=17,1,16`），否则数字不可复现。这也顺便
说明了一件事：**benchmark 的输入是实验的一部分**，不是随便挑的。

### 3. 栈访问会被误当成共享访问

第一版把非 global 的访存全记成 `shm_ld`，于是 `matmul_naive` 也报
`shm_ld=160` —— 那是 `@mul` 调用产生的栈溢出/恢复（`st.local` / `ld.local`），
不是共享内存。

第 18 章的 `_unit()` 把 space 0/1 之外**全部**归为 LSU，这在"发射槽"层面
是对的（都是访存），但在"流量"层面会误导。所以本章把统计拆成三档：

```python
{0: "glb_ld", 1: "shm_ld"}.get(space, "loc_ld")
```

拆完之后 `matmul_naive.shm_ld == 0` 才成立，门禁第 2 组也才敢断言。

### 4. tiling 不减少 load 次数

看统计：naive 288 次 load，tiled 320 次——tiled **更多**。它减少的是
`glb_ld`（128 → 32），同时把 `shm_ld` 从 0 拉到 128。

如果只盯 `loads` 这个总数，会得出"tiling 更差"的结论。所以统计必须能按
空间拆开：**一个总数不足以描述一次优化。**

### 5. `barriers=1` 与 `bar=2`

`bar=2`：两条 `bar.sync` 指令被执行（每个 warp 一条）。
`barriers=1`：block 放行了一次。

如果 2 个 warp 中有一个在 barrier 前 `exit`（第 17 章的选择），仍然是
`bar=1, barriers=1`。这两个字段任何单独一个都无法说明同步发生了什么，
必须配对读。

## 练习

1. **8×8**。把矩阵扩到 8×8、32 条 lane（4 个 warp），预测 `glb_ld` 的
   比值（提示：每个元素被读几次？）再实测。
2. **多 tile**。加一个 `k` 方向的 tile 循环（4×4 分块累加），让 tiling
   真正减少**计算**前的 global 读取次数，而不只是换空间。
3. **coalescing**。本章的模型里每次 lane 访存都算 1 次，不区分是否合并成
   事务。给模型加"每 8 条连续地址算 1 个事务"的规则，重算两个 kernel 的
   事务数，并说明为什么 naive 的列访问会退化。
4. **occupancy**。用 `--regs-per-thread` 喂入 `@mul` 的真实寄存器占用，
   算出 `warps_per_sm`，再讨论 2 warp 的 block 能放几个。
5. **反例**。把 tiled 的 barrier 删掉，用 `--schedule greedy` 跑，
   确认输出错在哪几个元素上，并解释为什么是那几个。

## 下一章

MVP 到此收口。剩下的不是新功能，而是把边界写清楚：

- `docs/README.md` 给出章节索引与"如何验证"；
- target `README.md` 写明这不是完整 GPU、不是 cycle-accurate；
- 8 项门禁全部 PASS，作为可复现的验收证据。

可选方向（不在 MVP 内）：

- **coalescing 模型**：把访存事务数从"lane 次数"里分离出来；
- **warp 级原语**：`shfl.sync`，让 warp 内交换不必经过共享内存；
- **真实硬件对照**：附录 A.4 的 GPU 解释器，用同一个 ELF 做差分测试。
