# 第 15 章：SIMT 验收——三个程序与执行统计

## 目标

第 11–14 章各修一块：模型、执行器、启动、分歧。每一章都有自己的门禁，
但没有一章回答"这些东西**一起**用对不对"。本章做验收：

1. 三个程序覆盖三种形状：`vecadd`（寄存器 ABI + 2 block）、`saxpy`
   （launch ABI + 无乘法器的 shift-add）、`reduce`（每 lane 不同圈数）；
2. 模拟器新增 `--stats`：`issued` / `lane_instrs` / `loads` / `stores` /
   `branches` / `splits` / `frames_max`；
3. 统计不是装饰：门禁检查 "grid 翻倍 → `issued` 翻倍" 和
   "uniform 程序 `lane_instrs == 8 * issued`，分歧程序 `less`"；
4. 给出后续章节的基线数字（第 16/17 章加 shared/barrier，第 18/19 章
   加调度与 occupancy，第 20 章做性能对比）。

退出条件（可执行）：

```sh
llvm/lib/Target/GPUMin/scripts/check-ch15.sh <llvm-bin-dir>
```

## 背景

### 为什么需要一章"验收"

前 14 章的门禁都是**局部**的：`check-ch06.sh` 只验内存指令，`check-ch12.sh`
只验执行器，`check-ch14.sh` 只验分歧。它们合在一起仍然可能错：

- 参数块 ABI 在 2 block 时算出错误的 `gid`；
- 分歧在"循环 trip count 不同"时的重汇聚点算错；
- 统计（如果有）把 lane 级事件按指令级计。

验收章的做法是：**用三个不同形状的程序跑同一条管线**，再对统计加不变量。

### 为什么是这三个程序

| 程序 | ABI | 覆盖 | 关键形状 |
| --- | --- | --- | --- |
| `vecadd` | `r4-r11` 寄存器 | 内存、`tid`、边界检查 | 2 block × 8 thread |
| `saxpy` | launch descriptor | 参数块、shift-add、`blockid` | `3*x + y`，无乘法器 |
| `reduce` | launch descriptor | 循环、分歧、重汇聚 | lane 0-3 八圈，4-7 七圈 |

`vecadd` 复用第 12 章的程序，但用第 13 章的两 block 形状跑；`saxpy` 和
`reduce` 是本章新增的。

### 为什么统计放在模拟器里

"结果对"不等于"执行对"。两个例子：

- 一个被 if-conversion 消掉的分支，结果完全正确，但 `branches` 会掉到 0；
- 一个被 CSE 合并的 `sr.activemask`（第 14 章那个坑），结果对、mask 错。

计数器是**低成本的可观测性**：不打断点、不改程序，就能看出执行形状变了。
第 18 章做调度模型、第 19 章算 occupancy、第 20 章比较 tiling 收益，都要
用到同一套计数。

## 设计

### 统计字段

| 字段 | 含义 | 计数单位 |
| --- | --- | --- |
| `issued` | 取指/发射的指令条数 | 指令（帧步） |
| `lane_instrs` | 实际执行的 lane-指令数 | lane × 指令 |
| `loads` / `stores` | 访存次数 | lane × 访存 |
| `branches` | 执行的 `bra` 条数 | 指令 |
| `splits` | 其中真正拆帧的次数 | 指令 |
| `frames_max` | 同时存在的最大帧数 | 帧 |

前两个的关系是整个 SIMT 模型的缩影：

```text
lane_instrs = Σ over issued steps of popcount(frame mask)
```

- 全 warp 一起走：`lane_instrs = issued * warp_size`；
- 分歧时：一部分步骤只有子集活跃，`lane_instrs < issued * warp_size`。

于是 "分歧率" 可以定义为：

```text
divergence_ratio = 1 - lane_instrs / (issued * warp_size)
```

这就是后面讲 occupancy 和 warp 效率时要用的一阶指标。

### 统计怎么采集

`_run_warp` 每步 `select()` 之后：

```python
self.stats["issued"] += 1
self.stats["lane_instrs"] += bin(warp.mask).count("1")
if len(warp.frames) > self.stats["frames_max"]:
    self.stats["frames_max"] = len(warp.frames)
```

分支处：

```python
self.stats["branches"] += 1
if taken and not_taken:
    self.stats["splits"] += 1
```

访存按 **lane** 记，因为一次 `ld.global.b32` 在分歧时只服务活跃 lane：

```python
lanes = warp.active_lanes()
self.stats["loads" if op == 0x08 else "stores"] += len(lanes)
```

### 输出到 stderr

`--stats` 写 **stderr**，`--dump` 写 stdout。理由很实际：门禁和脚本比较
的是 stdout 的数字，统计行混进去会破坏所有已有比较；同时 `--stats` 和
`--dump` 可以任意组合。

```sh
python3 tools/gpumin_sim.py prog.o --kernel k --launch k.json --stats
# stderr: issued=50 lane_instrs=400 loads=96 stores=16 branches=2 splits=0 frames_max=1
# stdout: 13 18 23 ...（dump 值）
```

### 验收矩阵

| 检查 | vecadd | saxpy | reduce |
| --- | --- | --- | --- |
| 值正确 | ✓ | ✓ | ✓ |
| `issued` | 26 | 50 | 82 |
| `lane_instrs` | 208 (=26×8) | 400 (=50×8) | 624 (<82×8) |
| `loads` | 32 | 96 | 92 |
| `stores` | 16 | 16 | 8 |
| `splits` | 0 | 0 | ≥1 |
| `frames_max` | 1 | 1 | 2 |

`reduce` 的 `lane_instrs = 624`、`issued * 8 = 656`，差 32 条就是分歧的
代价：lane 4-7 少跑一轮循环，那几轮的掩码只有 `0x0f`。

## 实现

### 模拟器

`Simulator.__init__` 建统计字典，`_run_warp` 采集，`report()` 格式化：

```python
def report(self):
    s = self.stats
    return ("issued=%d lane_instrs=%d loads=%d stores=%d branches=%d "
            "splits=%d frames_max=%d" % (...))
```

CLI 在 dump 之后打印：

```python
if args.stats and args.kernel:
    print(sim.report(), file=sys.stderr)
```

计数器放在 `Simulator` 而不是 `Warp` 上，因为它跨 warp/block 累加；
清零点就是一次 `run()`。

### 三个程序

**`saxpy.ll`**：没有乘法器，`3*x` 是 `(x << 1) + x`：

```llvm
%x2 = shl i32 %x, 1
%x3 = add i32 %x2, %x
%s  = add i32 %x3, %y
```

这不是模拟器的变通，是 ISA 的事实：GMVP 的 ALU 只有
`add/sub/and/or/xor/shl/shr/sar`。门禁里 `grep '\bshl\b'` 就是钉住
"用移位实现乘 3"。

**`reduce.ll`**：每个 lane 做自己的那部分求和：

```llvm
head:
  %i   = phi i32 [ %t, %entry ], [ %next, %body ]
  %acc = phi i32 [ 0, %entry ], [ %sum, %body ]
  %c   = icmp ult i32 %i, %n
  br i1 %c, label %body, label %done
body:
  %v   = load i32, ptr addrspace(1) %pi
  %sum = add i32 %acc, %v
  %next = add i32 %i, %stride
  br label %head
```

`n = 60`、`stride = 8` 时，lane 0-3 跑 8 圈、lane 4-7 跑 7 圈。真正的
block 归约（lane 之间通信）要等第 16 章的 shared memory 和第 17 章的
barrier；本章只做"每 lane 的那一份"。

### 运行

```sh
llc -march=gpumin -O1 -filetype=obj e2e/reduce.ll -o /tmp/reduce.o
python3 tools/gpumin_sim.py /tmp/reduce.o --kernel reduce_kernel \
        --launch e2e/reduce.json --stats
```

```text
issued=82 lane_instrs=624 loads=92 stores=8 branches=16 splits=1 frames_max=2
224 232 240 248 196 203 210 217
```

## 验证

```text
PASS 1/4: 2 blocks x 8 threads add correctly and report 32 loads/16 stores
PASS 2/4: saxpy computes 3x + y through the launch descriptor ABI
PASS 3/4: per-lane strided sums diverge and reconverge
PASS 4/4: statistics scale with the grid and expose divergence
```

四组分别覆盖：

1. `vecadd` 2×8 的值，以及 `loads = 32` / `stores = 16` 的精确计数；
2. `saxpy` 的值（`13 + 5g`）、`loads = 96`（每 lane 4 次参数 + 2 次数据），
   以及汇编里确实有 `shl`；
3. `reduce` 的 lane-varying 求和、`splits >= 1`、`frames_max == 2`；
4. 统计的不变量：`grid=1` 的 `issued` 恰好是 `grid=2` 的一半；uniform
   程序 `lane_instrs == 8 * issued`；分歧程序 `less`。

总验收仍是 8 项门禁，第 6 项现在跑第 12–15 章：

```text
Gates: 7 passed, 0 failed, 1 skipped
```

第 7 项要等第 18 章。

## 常见坑

1. **统计打到 stdout**。会污染 `--dump` 的比较；用 stderr。
2. **`lane_instrs` 按指令条数算**。忘了乘 `popcount(mask)`，分歧就看不
   出来了。
3. **访存按指令数算**。一条 `ld` 在分歧时只服务 4 个 lane，记 1 还是 4
   是两种含义；本章按 lane 记，因为后面算带宽要用它。
4. **`frames_max` 在 `_normalize` 之前取**。正在合并的帧会让峰值虚高；
   本章在 `select()`（内部先归一化）之后取。
5. **拿统计当周期**。这里没有流水线模型；`issued` 是条数，不是 cycle。
   第 18 章的调度模型才有"资源占用"的概念。
6. **`saxpy` 直接写 `mul`**。ISA 没有乘法器：前端会生成 `mul`，后端
   选不出来。要么写 `shl+add`，要么接受溢出语义写多步加法。
7. **`reduce` 把 trip count 写死成 8**。那就回到 uniform，`splits = 0`，
   这一章最重要的检查就失效了。
8. **grid 翻倍但 `issued` 不是 2×**。只有当 kernel 的每个 block 执行
   路径完全相同时才成立；如果 kernel 依赖 `blockid` 分支，它本来就不
   应该相等。
9. **忘了 `--stats` 和 `--kernel` 的顺序**。`--stats` 只在真的运行了
   一个 kernel 时才有输出（`--dump-text` 不执行）。
10. **把 `lane_instrs` 当"有效工作"**。它包含控制流指令；真正的"有用
    工作"要看具体 kernel 的语义计数（第 20 章会定义）。

## 练习

1. 给统计加 `cycles`，假设每条指令 1 周期、分歧时按帧串行；对比
   `lanes_efficiency = lane_instrs / (issued * 8)`。
2. 把 `reduce` 的 `n` 改成 57，算出每个 lane 的 trip count，预测
   `lane_instrs` 再跑一遍验证。
3. 写一个 `if (t & 1)` 的 kernel，让分歧率为 50%，验证
   `divergence_ratio = 1 - lane_instrs/(issued*8)` 接近 0.5。
4. 用 `--stats` 对比"if-conversion 成 `selp`"和"保留 `bra`"两种代码，
   解释 `branches` 和 `lane_instrs` 为什么一个降一个升。
5. 给 `vecadd` 加 `n = 12`（2 block × 8 thread），观察 `splits` 和
   `frames_max`。
6. 把三个程序的 `loads` 除以 `issued`，讨论"每指令访存率"对带宽的
   意义（第 20 章会用）。

## 下一章

第 16 章加 **shared memory**：`addrspace(2)`、静态 shared 全局、`sr`
基址读取。`reduce` 会从"每 lane 各算一份"变成"block 内真归约"，
而 `lane_instrs`/`frames_max` 会第一次出现"整个 block 一起参与"的形状。
