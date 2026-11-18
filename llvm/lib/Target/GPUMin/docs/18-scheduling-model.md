# 第 18 章：调度模型（ALU / LSU / BAR）

## 目标

第 15 章的 `--stats` 能说清"执行了多少条指令"，但回答不了"用了多少个周期"。
本章把调度量化：

1. 每条指令归到三类执行单元之一：**ALU**（整数/谓词/分支/特殊寄存器）、
   **LSU**（全局与共享访存）、**BAR**（barrier）；
2. 每个周期每类单元有固定数量的发射槽；一个 warp 每周期最多发一条；
3. 模拟器变成周期驱动的：`cycles` 是模拟器自己的时钟，不再是"指令条数"；
4. 用它回答一个真实问题：**加 warp 到底有没有用**。

退出条件（可执行）：

```sh
llvm/lib/Target/GPUMin/scripts/check-ch18.sh <llvm-bin-dir>
```

## 背景

### `issued` 不是时间

第 15 章的统计里，`issued` 的语义是"发射了几条指令"。它很有用——能看出
分歧、能看出 grid 缩放——但它对下面这件事一无所知：

```text
warp 0:  ld.global ...      LSU
warp 1:  add ...            ALU
```

如果 LSU 和 ALU 是两条独立流水线，这两条指令可以**同一拍**发射；如果它们
共用一条发射口，就要两拍。指令条数不变，时间翻倍。

所以需要一个模型回答："给定指令组合和硬件宽度，要多少拍。"

### 三分类从哪来

分类不是随便切的，它对应三种**互不相干**的硬件资源：

| 单元 | 包含 | 为什么单独一类 |
| --- | --- | --- |
| ALU | `add/sub/and/or/xor/shl/shr/sar`、`movi/addi`、`setp`、`selp`、`bra`、`call`、`sr.*`、`exit/ret` | 纯寄存器和控制流，不碰内存 |
| LSU | `ld.*` / `st.*` | 要访问内存子系统，延迟和带宽都与 ALU 无关 |
| BAR | `bar.sync` | 不是计算，也不是访存；它让整个 block 停下来 |

`sr.shared` 和 `sr.tid` 归 ALU：它们是特殊寄存器读，走的是寄存器通路。
`sra.activemask` 同理。这个选择会影响数字，但不会影响结论——真实 GPU 里
它们也占 ALU/SFU 的发射口。

### 为什么"加 warp"不是线性提速

直觉是"4 个 warp 应该 4 倍快"。本章的模型会让你看到它不是，原因是：

- **发射口有限**：ALU 每拍只有 1 个槽，4 个 warp 抢一个槽；
- **warp 是 lockstep 的**：4 个 warp 同一时刻往往在**同一条**指令上，
  于是它们同时要 ALU、同时要 LSU，永远排不上另一条流水线；
- **barrier 是全局停顿**：所有 warp 必须到齐，最慢的那个决定时间。

第 19 章加内存延迟后，这三条会变成"occupancy 为什么重要"的正式答案。

## 设计

### 模型参数

参数放在 `simt-model.json`，不是写死在代码里：

```json
"scheduling": {
  "units": { "alu": 1, "lsu": 1, "bar": 1 },
  "note": "issue slots per cycle per unit; no memory latency until chapter 19"
}
```

三个 1 的模型是"单发射、三流水线"。它简单到可以手算，又足够暴露冲突。

命令行可以覆盖，用于实验：

```sh
gpumin_sim.py ... --unit-width alu=2      # 双发射 ALU
gpumin_sim.py ... --unit-width lsu=2      # 两个 LSU 口
```

### 一个周期里发生什么

```text
for each cycle:
    slots = {alu: 1, lsu: 1, bar: 1}
    for each live warp in order:
        unit = classify(next instruction)
        if slots[unit] == 0:  continue        # 单元忙，下拍再试
        slots[unit] -= 1
        execute one instruction
    if all live warps are parked:
        release the barrier
```

三条隐含规则：

1. **一个 warp 每拍最多一条**。它由"for 循环里每个 warp 只出现一次"保证。
2. **被单元挡住的 warp 不算停住**。它下拍还会再试，不是 barrier 那种等待。
3. **barrier 放行不占发射槽**。放行发生在所有 warp 都到的下一拍。

### 为什么 `cycles` 只在 `units` 模式有意义

第 17 章的 `interleaved` / `greedy` 是两种**顺序**，不是两种**时钟**：

```text
interleaved: 一轮 = 每个 warp 各发一条   -> 不是一拍
greedy:      一轮 = 一个 warp 跑到停     -> 更不是一拍
```

所以那两个模式下 `cycles` 报 0：没有时钟模型，就不假装有。这个约定本身
就是教学点——**统计字段的含义必须由模型定义，而不是由名字暗示**。

## 实现

### 1. 分类器

```python
def _unit(self, word):
    op = word >> 26
    if op in (0x08, 0x09):
        return "lsu"
    if op == 0x12 and (word & 0x7FF) == self.model.barrier_funct:
        return "bar"
    return "alu"
```

分类只看编码，不看上下文。三个 SYS 变体里只有 `bar.sync` 是 BAR，其余
（`exit`、`ret`、`sr.*`）都是 ALU——这条规则必须和 `.td` 里的 funct 表一致，
`barrier_funct` 从模型读就是为了不出现第二份数字。

### 2. 预看下一条指令

调度器要在**执行之前**知道 warp 会占用哪个单元，所以要能"看一眼不前进"：

```python
def peek(self):
    """The frame that would run next, without moving the warp."""
    self._normalize()
    return self.frames[0] if self.frames else None
```

`_normalize()` 会合并同 PC 的帧，这是幂等的，所以 `peek()` 不会改变执行
语义。真实硬件做同样的事：取指阶段就知道 opcode，于是能判断进哪条流水线。

### 3. 周期计数放在哪里

`cycles` 加在 block 调度循环的顶部，而不是 `_run_warp` 里：

```python
while not all(done):
    if self.schedule == "units":
        self.stats["cycles"] += 1
        slots = dict(self.unit_width)
    ...
```

一条指令可能让一个 warp 退出（`exit`），可能让它停在 barrier，也可能只是
普通前进；这些都不影响"这一拍已经过去了"。**周期属于 block，不属于 warp。**

### 4. 单元计数放在哪里

`alu/lsu/bar` 在 `_run_warp` 里、取到 word 之后立刻计：

```python
word = self.image.word(pc)
self.stats[self._unit(word)] += 1
```

放在这里的好处是它对三种调度都成立，于是有一条永远可查的不变式：

```text
alu + lsu + bar == issued
```

这条不变式比任何具体数字都重要：它保证"分类"是完备的，没有指令被漏掉或
重复计数。

## 验证

四组门禁，全部围绕"模型必须自洽，然后才有资格谈性能"：

| 组 | 断言 | 为什么重要 |
| --- | --- | --- |
| 1 | `alu+lsu+bar == issued`，`lsu*8 == loads+stores` | 分类完备，且与访存统计对得上 |
| 2 | 单 warp `cycles == issued` | 每 warp 每拍一条 |
| 3 | 4 warp：`max(alu,lsu,bar) <= cycles < issued` | 单元能重叠，但 lockstep 抢同一单元 |
| 4 | `alu=2` 缩短时间，`bar=4` 不变 | 只加宽瓶颈才有用（Amdahl） |

### 实测数字

`vecadd`（每个 lane 2 次 global load + 1 次 store）：

```text
threads   issued  cycles  alu  lsu
8           13      13    10    3
16          26      23    20    6
32          52      43    40   12
```

读法：

- 1 warp：`cycles == issued`，一条一拍，没有任何重叠空间；
- 2 warp：`23 < 26`，ALU 和 LSU 开始重叠；
- 4 warp：`43 < 52`，重叠更多，但**远没有到 `max(40,12)=40`**——因为
  4 个 warp 是 lockstep 的，它们永远同时要 ALU。

这就是"加 warp 的收益递减"的模型解释，而不是一句经验之谈。

### 加宽单元

`barrier.ll` 的 `rotate`（32 lane，ALU 52 / LSU 16 / BAR 4）：

```text
配置             cycles
默认              60
--unit-width alu=2   38
--unit-width lsu=2   57
--unit-width bar=4   60
```

- `alu=2` 大幅下降：ALU 是瓶颈；
- `lsu=2` 只降 3：LSU 不是瓶颈，但它是 lockstep 之外的第二个冲突点；
- `bar=4` 完全不变：4 条 barrier 分散在程序里，从来不是瓶颈。

第 4 组门禁断言的就是这两件事：**变快**和**不变**。

## 常见坑

### 1. `exit` 让周期多算一拍

第一版在 `exit` 里写 `warp.set_current([]); continue`。`continue` 回到
`_run_warp` 顶部，`steps=1` 已经用完，于是返回 `None`（"还没结束"）；下一
拍调度器再进来，`select()` 返回 False 才报 `done`。结果 1 warp 的
`cycles` 是 14 而不是 13。

修法是在退出时直接判定：

```python
if funct == 0:  # EXIT
    warp.set_current([])
    if not warp.frames:
        return "done"
    continue
```

教训：**统计错误往往来自控制流的边界，而不是计算公式。**

### 2. `cycles=0` 不是 bug

`--schedule greedy` 下 `cycles` 恒为 0。这不是漏了计数，而是拒绝在"没有
时钟的模型"里报时钟。如果门禁里把 `cycles=0` 当成失败，就会逼着实现去
编一个假数字——那比缺字段更糟。

### 3. lockstep 让"加宽 ALU"的收益被高估

`alu=2` 把 60 拍降到 38 拍，看起来是 1.58 倍，但 `ceil(52/2)=26` 才是理想值。
差额来自：两个 warp 同时要 LSU 时，LSU 只有 1 个槽，另一个 warp 只能干等，
它空出来的 ALU 槽没人填（因为所有 warp 都在同一条指令上）。

这也是第 19 章的入口：**只有让不同 warp 处在不同指令上（真实的
divergence/occupancy），空槽才会被填上。**

### 4. `sr.*` 归 ALU 是一个选择

如果把它归到一个独立的 SFU 单元，`rotate` 的 ALU 数会下降 8（每个 warp
2 条 `sr.*`），`cycles` 也会变。本章不这么做，因为：

- 模型里多一个单元就要多一条门禁断言；
- `sr.*` 在真实硬件上也是走 ALU 类通路（不是内存）。

但这是**记录在案的简化**，不是"显然如此"。第 19 章加 latency 时会重新
审视这条。

### 5. 单元宽度是模型，不是硬件事实

`units` 的三个 1 只是"单发射三流水线"的假设。真实 T4 每个 SM 有 4 个
warp scheduler、每拍每 scheduler 一条指令，还有独立的 LD/ST 单元。模型
的作用是让**结论对参数不敏感**：加宽瓶颈单元一定变快，加宽空闲单元一定
不变——这两条在参数变化下都成立，所以它们是模型的性质，不是数字的巧合。

## 练习

1. **手算 `vecadd`**。用 `.s` 的指令序列算出 4 warp 的 `cycles` 下界，和
   实测的 43 比较，解释差额。
2. **加一个 `--unit-width bar=0`**（禁止并发 barrier），观察 `cycles` 与
   `barriers` 的变化，说明为什么"0 槽"在模型里是不可实现的。
3. **把 `sr.*` 拆成独立单元**，改模型、改分类器、改门禁，重新测三组数字。
4. **验证 Amdahl**。构造一个 90% ALU / 10% LSU 的 kernel，把 ALU 加宽到
   4，预测并实测加速比。
5. **对比真实硬件**。用附录 A.4 的 GPU 解释器跑同一个 kernel，比较
   "指令数 / 周期数"的比值，解释为什么两者不可能相等。

## 下一章

本章的模型没有**延迟**：`ld.global` 发出去，下一拍结果就能用。真实 GPU
不是这样，所以：

- 第 19 章给 LSU 加延迟，warp 在结果可用前不能继续（没有 scoreboard，
  只能整 warp 停）；
- 于是 `occupancy`——一个 SM 上同时驻留多少 warp——第一次有了意义：
  停住的 warp 让出槽位，别的 warp 填进来；
- 那才是"为什么要有这么多 warp"的完整答案，也是第 20 章 matmul 性能验收
  的前提。
