# 第 19 章：延迟与 occupancy

## 目标

第 18 章的模型有一个明显不真实的地方：`ld.global` 发出去的下一拍，结果就
能用了。本章补上这一半：

1. **延迟**：一条 load 之后，warp 要等固定拍数才能继续；
2. **occupancy**：一个 SM 里能驻留多少 warp，由寄存器预算和硬件上限共同
   决定；
3. 用两者解释一个此前只能靠经验回答的问题：**为什么需要很多 warp**；
4. 默认延迟保持 0，让第 12–18 章的数字可以原样复现。

退出条件（可执行）：

```sh
llvm/lib/Target/GPUMin/scripts/check-ch19.sh <llvm-bin-dir>
```

## 背景

### 没有延迟的模型会给出错误结论

第 18 章里，"加 warp"的收益只来自单元重叠。按那个模型，4 个 warp 跑
`vecadd` 用 43 拍完成 52 条指令——已经接近 1 拍 1 条，再加 warp 没有意义。

真实硬件不是这样。一次 global load 的延迟是几百拍，而 ALU 只要有活就能发。
于是同一时刻：

```text
warp 0:  等 load 回来（几百拍，什么也不做）
warp 1:  ← 如果它在，就能用这几百拍算别的
```

occupancy 的全部意义就是：**让"停住"的 warp 让出槽位，给别的 warp 用**。
没有延迟的模型里没有"停住"，所以 occupancy 无处发挥。

### 为什么是"整 warp 停"

真实 GPU 有 scoreboard：load 发出后，warp 可以继续执行**不依赖**结果 的
指令，只在真正用到结果时停。

本章不实现 scoreboard，理由是它需要追踪每条指令的目的寄存器、以及在
寄存器级记 ready time——那是编译器寄存器分配和硬件记分牌的共同职责，
放在教学模拟器里会把代码量翻倍，而**结论不变**：

- 有 scoreboard：停顿变短，但仍在；
- 没有 scoreboard：停顿变长，但"多 warp 能填上"这件事一模一样。

所以本章取简单侧：**load 之后整个 warp 停 N 拍**，并在文档里写明这是简化。

### 延迟参数

```json
"latency": { "global": 0, "shared": 0,
             "note": "cycles a warp waits after a load; 0 keeps chapters 12-18 unchanged" }
```

默认 0 是一个刻意的选择，理由在"常见坑"第 5 条。

真实 T4 的数量级是：global 约 200–400 拍、shared 约 20–30 拍。教学用
`--latency 8`，因为 8 拍已经足以让"1 warp 慢、4 warp 快"显形，又不会让
一次运行变得很长。

### occupancy 是两个上限的最小值

```text
warps_per_sm = min( max_warps_per_sm,
                    registers_per_sm / (registers_per_thread * warp_size) )
```

参数放在模型里：

```json
"occupancy": { "registers_per_sm": 65536,
               "max_warps_per_sm": 64,
               "registers_per_thread": 32 }
```

两个上限的含义完全不同：

| 上限 | 来源 | 何时起作用 |
| --- | --- | --- |
| 64 warp | 硬件调度器能跟踪的 warp 数 | 寄存器用得少时 |
| 65536 / (rpt × 8) | 寄存器堆容量 | 每线程寄存器多时 |

在 32 寄存器/线程时，寄存器预算能放下 256 个 warp，所以上限是硬件的 64。
到 129 寄存器/线程时，预算只剩 63 个——寄存器开始成为瓶颈。

## 设计

### 状态机

每个 warp 多一个计数器 `stall`：

```text
         issue a load
running ----------------> stalled(stall = L)
   ^                          |
   |        stall == 0        |  每个周期 stall -= 1
   +--------------------------+
```

关键点：`stall` 是**周期**，不是指令。它只在 `units` 模式下生效，因为只有
那个模式有"一拍"的定义。

### 为什么放在 warp 上而不是 block 上

延迟是**单条 warp 的私有状态**：warp 0 在等 load 的时候，warp 1 完全正常。
如果把它放在 block 上，整个 block 就一起停了——那是 barrier 的语义，不是
内存延迟的语义。

这也是"occupancy 有用"的前提：block 里每个 warp 各自停各自的，调度器才有
别的 warp 可以发。

### 与 barrier 的关系

两种"停"必须区分开：

| | barrier | latency stall |
| --- | --- | --- |
| 停的是谁 | 整个 block | 一个 warp |
| 什么时候走 | 所有 live warp 到齐 | 计数到 0 |
| 能不能被别的 warp 填 | 不能（都在等） | 能（这就是 occupancy） |
| 统计字段 | `barriers` | `stall_cycles` |

`stall_cycles` 统计的是"warp-周期"：4 个 warp 各等 8 拍，记 32。它不是
"损失的周期数"——如果期间别的 warp 在发指令，一个周期都没损失。这一点在
"常见坑"第 3 条里还会再说一次。

## 实现

### 1. 计数器放在哪里递减

第一版把递减放在周期循环的开头，单独遍历一遍 warp：

```python
for i, warp in enumerate(warps):
    if done[i] or waiting[i] or not warp.stall:
        continue
    warp.stall -= 1
    self.stats["stall_cycles"] += 1
```

它错在**边界**：递减到 0 的那一拍，后面的发射循环看到 `stall == 0`，于是
同一拍就发了。实际等待变成 `L-1` 拍。

正确做法是把递减放进发射循环本身——它每个 warp 每拍恰好经过一次：

```python
if warp.stall:
    warp.stall -= 1
    self.stats["stall_cycles"] += 1
    continue
```

于是 load 在 C 拍发出、`stall = L`，C+1 … C+L 各减一次并跳过，
C+L+1 拍恢复发射：正好等 L 拍。

### 2. 延迟按地址空间取

```python
if op == 0x08 and self.schedule == "units":
    warp.stall = self.latency["global" if space == 0 else "shared"]
```

只有 load 会产生需要等待的值；store 是"发出去就不管"，因此不设延迟。这在
真实硬件上是写缓冲的简化版，但不影响本章的任何结论。

`space` 是编码里的两位字段（0=global、1=shared、2=local、3=constant），和
第 16 章的映射一致。

### 3. occupancy 只在初始化时算一次

```python
self.warps_per_sm = self.model.warps_per_sm(regs_per_thread)
```

它不参与执行，只进统计。这一章要证明的是"这个数是由寄存器预算决定的"，
而不是"模拟器按 occupancy 调度"——后者需要多 block 驻留模型，是 MVP 范围
之外的事（见练习 2）。

## 验证

四组门禁：

| 组 | 断言 | 说明 |
| --- | --- | --- |
| 1 | 1 warp：`cycles == issued + ld_ops*L`，`stall_cycles == ld_ops*L` | 延迟按拍记账 |
| 2 | 不给 `--latency` 时 `cycles == issued`、`stall_cycles == 0` | 历史可复现 |
| 3 | 1→2→4 warp，每指令周期数严格下降 | occupancy 的收益 |
| 4 | 32/129/255 寄存器/线程 → 64/63/32 warp | 两个上限谁在起作用 |

### 实测数字

`vecadd`，`--latency 8`：

```text
threads  issued  cycles  stall_cycles  cycles/instr
8          13      29        16          2.23
16         26      35        32          1.35
32         52      51        64          0.98
```

三行合起来就是本章的结论：

- 1 warp：13 条指令用了 29 拍，其中 16 拍在原地等 load；
- 2 warp：stall 累计翻倍（32 拍），但总周期只多 6 拍——有一半被另一个
  warp 填上了；
- 4 warp：64 拍 stall 只让总周期增加 8 拍，每指令周期数掉到 1 以下
  （ALU 与 LSU 重叠，部分指令同拍完成）。

对比第 18 章 latency=0 的 4 warp：43 拍。**延迟让同一个 kernel 从 43 拍到
51 拍，而 warp 从 1 增到 4 把 29 拍/13 条压到 51 拍/52 条。** 这两句话只有在
模型里有延迟时才能同时说出来。

occupancy：

```text
registers/thread   warps_per_sm   谁在限制
32                 64             硬件上限
129                63             寄存器预算（65536 / 1032）
255                32             寄存器预算（65536 / 2040）
```

## 常见坑

### 1. `L-1` 而不是 `L`

见"实现 1"。症状很隐蔽：`stall_cycles` 报的是 16（正确），`cycles` 却只多了
14。两个统计各自都自洽，加起来对不上——所以门禁第 1 组同时断言两件事：

```sh
[ "$cycles" = "$((issued + stall))" ]
```

**一条不变式比两个独立的期望值更能抓住边界错误。**

### 2. `stall_cycles` 不是损失的周期

`stall_cycles` 是"warp-周期"的总和。4 个 warp 各停 8 拍记 32，但如果这 32
个 warp-周期里始终有别的 warp 在发指令，损失的实际周期是 0。

所以不要写 `cycles == issued + stall_cycles`——那只在单 warp 时成立。门禁
第 1 组限定 `--threads 8`（一个 warp）正是为了让它成立。

### 3. "每指令周期数"可以小于 1

4 warp 时 `cycles/issued = 51/52 < 1`。这不是错：`issued` 数的是**指令**，
而每个周期最多能发 `alu + lsu + bar = 3` 条（三条流水线各一条）。所以
"cycles < issued" 恰恰说明单元在重叠。

第 18 章的门禁里有一条 `cycles < issued` 用来证明重叠发生；本章第 3 组用
的是**跨配置比较**（`c8/i8 > c32/i32`），而不是绝对比值，就是因为单点
比值没有意义。

### 4. `warps_per_sm` 不由模拟器调度

它只是算出来的一个数。真正的 occupancy 调度需要"一个 SM 上放多个 block、
block 之间争抢发射槽"的模型。本章不做，因为：

- MVP 的 launch 是 1D grid，block 之间没有共享状态；
- 一旦引入多 block 驻留，`barriers` 的语义要按 block 分开，统计口径全变。

所以本章的结论限定为："**给定每线程寄存器数，能驻留多少 warp**"，以及
"驻留越多，延迟越容易被填"。这两条不依赖多 block 模型。

### 5. 默认值是 API

`--latency` 默认 0，不是"忘了设成真实值"。如果默认改成 8，第 18 章门禁里
"单 warp `cycles == issued`"会立刻红，第 15 章的基线数字也全部失效。

一条统计字段的含义一旦被前一章的门禁依赖，它就**不能在新的一章里悄悄改
含义**。想引入更真实的模型，就加开关——这也是本章只加参数、不改默认的原因。

## 练习

1. **scoreboard**。给每个 warp 记"最近一次 load 的目的寄存器 + ready 拍"，
   只有后续指令读这个寄存器时才停。对比有/无 scoreboard 的 `cycles`，说明
   为什么真实硬件值得为它加复杂度。
2. **多 block 驻留**。让一个 SM 同时跑 `warps_per_sm / nwarps` 个 block，
   共享三个单元的发射槽。预测 `vecadd`（2 block）与单 block 的差别，再实测。
3. **按空间设延迟**。把 `--latency` 拆成 `--latency global=200,shared=25`，
   用 `rotate` 观察 shared 与 global 各自贡献多少拍。
4. **occupancy 的真实来源**。用 `llc -print-after=greedy` 读出每个 kernel 的
   实际寄存器数，喂给 `--regs-per-thread`，列出四个 e2e kernel 各自的
   `warps_per_sm` 与 `blocks_per_sm`。
5. **验证模型的极限**。把 latency 拉到 200，看 4 warp 与 32 warp（如果允许）
   的 `cycles/instr` 如何收敛。这个极限值就是"延迟完全被隐藏"的理论下界。

## 下一章

模型齐了：指令分类、发射槽、延迟、occupancy。第 20 章用它们做一次真正的
验收——**tiled matmul**：

- 一个 `C = A × B` 的 kernel，tile 进共享内存，block 内用 barrier 同步；
- 用 `--stats` 比较 naive 版与 tiled 版的 `loads`、`cycles`；
- 用第 18/19 章的模型解释收益从哪来（复用 vs 带宽）；
- 验收标准不是"跑对"，而是"数字能解释清楚"。

这也是 MVP 的最后一个功能章：之后只剩收口（README、计数、8/8 门禁复核）。
