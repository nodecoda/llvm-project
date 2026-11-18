# 第 17 章：屏障与 block 级调度

## 目标

第 16 章让 warp 内的 lane 通过共享内存交换数据，但**跨 warp** 的读写没有任何
顺序保证。本章加最后一块同步原语：

1. 一条新指令 `bar.sync`（SYS funct 11，无操作数）；
2. 模拟器从"warp 跑到底"升级成真正的 **block 调度器**：warp 可以在 barrier
   处停住，等所有 warp 到齐后一起放行；
3. 两种调度器 `interleaved`（默认）与 `greedy`，用来说明**"在一种调度下跑
   对"不叫正确**；
4. 一个跨 warp 的 rotate 程序：有 barrier 时两种调度都对，没 barrier 时
   `greedy` 必错、`interleaved` 对——这就是竞态的定义。

退出条件（可执行）：

```sh
llvm/lib/Target/GPUMin/scripts/check-ch17.sh <llvm-bin-dir>
```

## 背景

### 共享内存不带同步

第 16 章的 `shared_copy` 之所以正确，靠的是一个隐含前提：一个 warp 的 8 条
lane 在同一拍执行同一条指令。把 lane 换成 warp，这个前提立刻消失：

```text
warp 0:  tile[0..7] = ...          warp 1:  tile[8..15] = ...
                                     ^ 谁先执行？没有答案
```

共享内存只解决**地址可见性**（哪个 block、哪个偏移），不解决**时序**。硬件
不会因为两条指令访问同一块 SRAM 就自动排队——它只会按某个调度顺序发射它们。

### 三种"顺序"

把这件事拆成三层，后面所有讨论都建立在这上面：

| 层 | 问题 | 由谁决定 |
| --- | --- | --- |
| 程序序 | 同一 warp 内两条指令谁先 | 编译器 + 分支 |
| 调度序 | 不同 warp 的指令谁先 | 硬件调度器（或模拟器） |
| 内存序 | 一次写在什么时候对其他 warp 可见 | barrier + 内存模型 |

`bar.sync` 是唯一能在**程序**里约束第三层的东西：它说"我之前的共享写，在我
之后的共享读之前，对所有 warp 可见"。没有它，编译器不能假设任何顺序，硬件
也不提供任何顺序。

### 编码：为什么没有 barrier id

SYS 的 funct 占低 11 位，已经被 `exit`(0)、`ret`(1)、`sr.*`(2–10) 用掉。
第 02 章的 opcode 表里给 `bar` 留了下一个值：

```text
funct 11  ->  bar.sync
```

它没有操作数，整条指令就是"SYS + funct"。CUDA 的 `bar.sync id, count` 有
屏障编号和线程数，这里**故意不做**：

- 编号的意义是同一 block 内多组 warp 各等各的，需要 launch 侧能划分 warp 组；
- 线程数的意义是允许部分线程不参与，需要计数寄存器。

两件都需要运行时的配合，而 MVP 的 launch 只描述 grid/block/args。把它们留成
"以后加一个带立即数的新 opcode"，比现在做一个半成品更诚实。

### 为什么是 effectful + convergent

`bar.sync` 必须满足两条性质，缺一条就会在优化里出事：

| 性质 | 没有它会怎样 |
| --- | --- |
| 有副作用（effectful） | 两条相邻的 barrier 被 CSE 成一条；整条被 DCE |
| 收敛（convergent） | 被 hoist 出分支、被 duplicated 到两条路径 |

第 14 章已经踩过同一个坑：`sr.activemask` 必须 effectful，否则分支里的三次
读取会被合并到 entry。barrier 更严重——被合并意味着少了一次同步，程序在
`interleaved` 下可能还对，在 `greedy` 下直接错。

在 `.td` 里两条都要写：

```tablegen
def BAR_SYNC : GPUMinInst<(outs), (ins), "bar.sync",
                          [(int_gpumin_barrier)]> {
  let Inst{31-26} = OPC_SYS.Value;
  let Inst{10-0} = 11;
  let hasSideEffects = 1;
  let isConvergent = 1;
}
```

IR 侧是 `llvm.gpumin.barrier()`，声明成 `Intrinsic<[], [], [IntrHasSideEffects,
IntrConvergent]>`。没有返回值，所以 DAG 节点是纯 chain——TableGen 看到
`IntrHasSideEffects` 会自动生成带 chain 的匹配模式，不需要手写第二个模式。

## 设计

### barrier 的语义

一句话：**当 block 内所有仍然活着的 warp 都到达 barrier 时，它们一起继续。**

三个细节都是**选择**，不是"硬件显然如此"：

1. **整 warp 到达**。barrier 是 warp 级指令，一个 warp 的所有活跃 lane 同时
   到达；本章不做 lane 级 barrier。
2. **已退出的 warp 不再阻塞**。这样"部分 warp 先 `exit`、其余 warp 继续
   归约"是合法的。
3. **不做内存模型细化**。barrier 之后的共享读看到 barrier 之前的共享写，
   仅此而已。

第 2 条是本章唯一一处与 CUDA 明确不同的地方。CUDA 里"有些线程先退出、
其余线程再进 `__syncthreads()`"是未定义行为，实践中会挂住。教学模型选
"退出即离开屏障组"，因为：

- 它让归约类 kernel 能写出"少于一个 block 的线程"的形状；
- 它可判定：模拟器只需要数"还活着的 warp"；
- 它是可测的：门禁里没有依赖它的程序，但模型文件写明了。

在 `simt-model.json` 里它是数据，不是注释：

```json
"barrier": { "name": "bar.sync", "funct": 11, "scope": "block" }
```

### 两种调度器

模拟器有两种把指令发给 warp 的方式，命令行切换：

| 调度 | 行为 | 用途 |
| --- | --- | --- |
| `interleaved`（默认） | 每轮每个可运行 warp 执行 **1 条** | 接近真实硬件的 warp 交错 |
| `greedy` | 一个 warp 跑到结束或停在 barrier，再换下一个 | 最坏情况：把竞态变成必然 |

两者都是**合法调度**。这一点是本章的核心：程序必须在两种调度下都对。

`greedy` 不是"错误模型"，它是"某个合法顺序"。真实 GPU 在某些 occupancy
和 warp 数量下确实会长时间只推进少数 warp。用最坏顺序做验收，比用平均顺序
更可靠。

### 调度器状态机

```text
        +---------+   bar.sync    +---------+
        | running | ------------> | waiting |
        +---------+               +---------+
             |  exit                   ^
             v                         | 全部 live warp 都 waiting
        +---------+                    |
        |  done   |          release --+
        +---------+
```

release 条件写成一行：

```python
if all(done[i] or waiting[i] for i in range(n)):
    waiting = [False] * n        # 全组放行
```

`done` 的 warp 等价于"已经到达所有未来的 barrier"，所以不参与等待。

## 实现

### 1. 让 `_run_warp` 可恢复

第 12–16 章的 `_run_warp` 是"跑到帧栈空为止"。barrier 要求它在中间停下来，
所以它现在返回一个状态：

```python
def _run_warp(self, warp, steps=None):
    """Run until the warp finishes ("done"), parks at a barrier, or has
    executed `steps` instructions."""
```

- `steps=1`：`interleaved` 用，执行一条就返回 `None`；
- `steps=None`：`greedy` 用，跑到 `done` 或 `barrier`。

barrier 的处理只有三行：

```python
if funct == self.model.barrier_funct:  # BAR.SYNC
    warp.advance()
    return "barrier"
```

注意 `warp.advance()` 在**停住之前**：warp 的 PC 已经指向 barrier 后面那条
指令，放行时直接从那里继续，不会重复执行 barrier。

### 2. block 调度器

```python
while True:
    runnable = False
    for i, warp in enumerate(warps):
        if done[i] or waiting[i]:
            continue
        runnable = True
        steps = 1 if self.schedule == "interleaved" else None
        status = self._run_warp(warp, steps=steps)
        if status == "done":
            done[i] = True
        elif status == "barrier":
            waiting[i] = True
    if not runnable:
        if any(waiting):
            self.stats["barriers"] += 1
            waiting = [False] * n
            continue
        return
    if all(done[i] or waiting[i] for i in range(n)):
        ...
```

`interleaved` 模式下 `_run_warp` 每次只走一条指令，所以内层 for 循环就是
"一轮里每个 warp 各发一条"；`greedy` 模式下一次调用就走到底，于是回到第
12 章的行为，只是多了 barrier 这个出口。

统计新增 `barriers`：**放行次数**，不是 `bar.sync` 的执行次数。两者在
单 barrier 程序里都是 1，但在"两个 barrier 背靠背"时会不同——那是练习 2。

### 3. 一个跨 warp 的程序

`scripts/e2e/barrier.ll`：

```llvm
store i32 %v, ptr addrspace(2) %pt      ; 每个 lane 写自己的槽
call void @llvm.gpumin.barrier()
%src = and i32 (add i32 %t, 8), 31      ; 读下一个 warp 的槽
%w = load i32, ptr addrspace(2) %ps
store i32 %w, ptr addrspace(1) %pout
```

`tid` 0–31 分到 4 个 warp，lane `t` 读的槽由 `t+8` 写——正好是**相邻 warp**。

编译出来 barrier 落在 store 和 load 之间：

```asm
	st.shared.b32 r4, 0(r7)
	addi r2, r2, 8
	movi r4, 31
	and r2, r2, r4
	shl r1, r2, r1
	add r1, r1, r6
	bar.sync
	ld.shared.b32 r1, 0(r1)
	st.global.b32 r1, 0(r3)
```

地址计算被调度到 barrier **下面**了。这不违反语义：barrier 约束的是共享
访存，不是纯算术。第 18 章的调度模型会决定这类移动值不值。

## 验证

四组门禁：

| 组 | 断言 | 说明 |
| --- | --- | --- |
| 1 | 模型/汇编器/选择器给出同一个 `bar.sync`，两条不合并 | 编码与副作用 |
| 2 | `st.shared` < `bar.sync` < `ld.shared` | barrier 在它该在的位置 |
| 3 | 两种调度下 rotate 都对，`barriers=1` | 有 barrier 的语义 |
| 4 | 去掉 barrier：`interleaved` 对、`greedy` 错 | barrier 不可省 |

第 4 组是本章的重点。同一份不带 barrier 的 IR，两种调度给出：

```text
interleaved:  108 109 110 ... 131 100 101 ... 107     (正确)
greedy:       0 0 0 0 0 0 0 0 ... 0 0 0 0 100 ... 107 (错误)
```

`greedy` 下 warp 0 一口气跑完，此时它要读的槽还没人写；warp 3 读的是 warp 0
的槽，而 warp 0 早就跑完了，所以最后 8 个值是对的。这个"部分正确"的输出比
全错更能说明问题：**程序的行为变成了调度器的属性**。

统计数字：

```text
有 barrier:  issued=72 lane_instrs=576 loads=64 stores=64 barriers=1
无 barrier:  issued=68 lane_instrs=544 loads=64 stores=64 barriers=0
```

差 4 条指令 = 每个 warp 一条 `bar.sync`。`lane_instrs` 差 32 = 4 warp × 8 lane，
说明 barrier 也是**按 warp** 计的一条指令，不是按 lane。

## 常见坑

### 1. `i1` 不能当函数参数

第一版测试里写了 `define void @in_branch(i1 %c)`，`llc` 直接崩：

```text
LLVM ERROR: unable to allocate function argument #0
```

GPUMin 的 `LowerFormalArguments` 只处理整数/指针宽度 ≥ 16 的实参，没有 i1
的寄存器类。这不是本章引入的问题，但很容易在写测试时撞上。修法是让条件来自
计算：

```llvm
%c = icmp eq i32 %x, 0
br i1 %c, label %then, label %end
```

### 2. `barriers` 与 `bar.sync` 次数不是一回事

`barriers` 数的是**放行次数**。两个连续的 `bar.sync` 如果所有 warp 都同步
推进，会得到 `barriers=2`；但如果一个 warp 在第一个之后退出了，放行条件
就会变成"剩下的 warp 都到了"，仍然放行两次。想做更细的统计（每个 barrier
多少 warp 到达）需要给每个 barrier 编号——那是练习 2。

### 3. 已退出 warp 的两种语义都能自洽

本章选"退出即离开屏障组"。如果改成"退出的 warp 永远算到达"，归约程序和
"提前返回"的 kernel 会死锁。反过来，如果要求"所有 warp 必须执行同样次数
的 barrier"，那 `exit` 早于 barrier 就是编译错误，需要 CFG 级检查。

三个选择都能写出一致的模拟器，**代价是它们互相不兼容**。所以第 1 条里的
那句话要写进模型文件，而不是散在代码注释里。

### 4. 调度器会把纯算术挪过 barrier

见上面的汇编：`addi/and/shl/add` 跑到了 `bar.sync` 下面。这不是 bug，但
它提醒一件事：**barrier 不是"指令栅栏"**。想让某条指令严格在 barrier 之前
完成，只能让它有副作用（写共享内存、写全局内存）。

### 5. 一种调度下的正确性证明不了什么

如果没有第 4 组门禁，`rotate` 去掉 barrier 后在默认调度下**完全正确**，
CI 会一直绿。竞态在"最坏顺序"下才现形，所以验收必须跑最坏顺序。这条对
后面第 20 章的 matmul 同样适用。

## 练习

1. **lane 级 barrier**。给 `bar.sync` 加一个 mask 操作数，只同步指定 lane
   子集，说明模拟器的 `waiting` 需要从 warp 级降到帧级。
2. **barrier id**。把 funct 11 之后再加两个 funct（12/13）表示 barrier 0/1，
   让一个 block 里的两组 warp 各等各的，验证 `barriers` 计数与放行次数。
3. **deadlock 检测**。构造"一个 warp 在 barrier 前 `exit`、另一个 warp 永远
   循环"的程序，让模拟器在 `max_steps` 之前报"所有 live warp 都在等待但没有
   放行"。注意：本章的模型下这**不应**死锁，所以这个练习真正的产出是
   解释为什么。
4. **加 `--schedule random`**。用固定种子随机挑 warp 发射，跑 100 次，
   统计 `rotate`（有/无 barrier）各自有多少次正确。这是对第 4 组门禁的
   概率版补充。
5. **把 barrier 提到循环外**。写一个循环里带 `bar.sync` 的 kernel，确认
   `isConvergent` 阻止了 LICM 把它提出去（看 IR pass 的输出）。

## 下一章

同步语义齐了，接下来是**性能模型**。第 18 章把每条指令归到三类执行单元：

- **ALU**：整数运算、谓词、分支；
- **LSU**：全局/共享访存；
- **BAR**：barrier。

然后模拟器按"每拍每类单元能发几条"来调度 warp，`issued` 的含义从"指令数"
变成"周期数"，`barriers` 变成一个真正的停顿来源。到那时本章的 `greedy` /
`interleaved` 会退化成同一个模型的两个极端参数，而 `--stats` 的输出会第一次
能用来比较两个 kernel 的快慢。
