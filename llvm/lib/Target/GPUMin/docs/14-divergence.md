# 第 14 章：warp 内分歧——帧集合与动态重汇聚

## 目标

第 12 章的执行器有一个硬约束：**一个 warp 只有一个 PC**，分支必须
warp 内一致，否则直接报 `non-uniform branch`。这让 `sum_to(tid)` 这种
"每个 lane 跑不同圈数"的程序一行都跑不了。

本章把那条约束拆掉：

1. 一个 warp 的活跃状态从 `(pc, mask)` 变成**帧集合** `[(pc, mask), ...]`；
2. 分支时按 take/not-take 拆帧，两半各自往前走；
3. 调度规则是**取最小 PC 的帧执行**，同 PC 的帧自动合并——这就是
   动态重汇聚点，不需要静态 IPDOM；
4. `ret` 也要拆帧，因为不同 lane 可以返回到不同的 `lr`；
5. `exit` 只结束**当前帧**的 lane，不动其他帧；
6. `sr.activemask` 读到的是**当前帧的 mask**，不是整 warp 的；
7. `--strict-uniform` 把第 12 章的语义原样保留下来，作为回归开关。

顺带补上编译器侧被这次测试暴露的三个缺口（`trunc i32 -> i1`、i1 分支
条件、`activemask` 必须按 effectful 算）。

退出条件（可执行）：

```sh
llvm/lib/Target/GPUMin/scripts/check-ch14.sh <llvm-bin-dir>
```

## 背景

### 分歧是 SIMT 的常态，不是异常

一个 warp 的 8 个 lane 共用取指，但各自有寄存器和谓词。一旦分支条件
依赖 lane 自己的数据：

```llvm
%t = call i32 @llvm.gpumin.tid()
%low = icmp slt i32 %t, 4
br i1 %low, label %lowarm, label %higharm
```

lane 0-3 想走 `lowarm`，lane 4-7 想走 `higharm`。取指部件一次只能取
一条指令，所以硬件必须选一边先跑，把另一边的 lane **挂起**，跑完再换。

第 12 章的做法是拒绝执行。问题在于：真实 kernel 里 `if (t < n)`、
`while (i < n)`、递归深度不同，全是这个形状。不实现分歧，模拟器就只能
跑 toy 程序。

### 静态 SIMT stack 的问题

教科书写法是 **SIMT stack**：给每个 warp 维护一个栈，元素是
`(reconvergence pc, active mask, next pc)`；遇到分歧就压栈，跑到
IPDOM（immediate post-dominator）就出栈。

它对**结构化**控制流很漂亮，但有三处麻烦：

| 情况 | 静态 IPDOM 的问题 |
| --- | --- |
| `ret` 到不同返回点 | 返回值是数据，不是 CFG 边，算不出汇合点 |
| 提前 `exit` / `break` | 有的 lane 永远到不了汇合点 |
| 不规则 CFG（switch、indirect） | 需要额外构造 post-dominator 树 |

我们的教学后端里，第 07 章已经有 `call/ret`，第 12 章的递归程序
（`fib`）就在用。硬套 IPDOM 会逼着先实现一套完整 CFG 分析，收益却
只是"看起来更像硬件"。

### 动态重汇聚：帧集合

本章换一个等价但更小的模型：

> warp 的活跃状态是若干 **帧** `(pc, mask)`。
> 每步取 **pc 最小** 的帧执行；pc 相同的帧在归一化时合并。

为什么这是对的：

- 每帧内部所有 lane 在同一个 pc，和"整 warp 一起走"没有区别；
- 帧之间互不干扰：lane 的寄存器、谓词、私有内存都是 lane 私有的，
  连 `lr` 也是（第 07 章把它放在 `r31`）；
- 两个 lane 到达同一个 pc 时，它们的后续执行**完全由各自状态决定**，
  所以合并成一个 mask 的帧，与顺序执行两个帧等价；
- "取最小 PC" 保证：先到达汇合点的一支会**停在那里等**，因为另一支
  还在更小的 pc 上。等两支同 pc 时，`_normalize` 自然把它们并成一帧。

这就是**动态重汇聚**：不预先算汇合点，靠"最小 PC + 同 PC 合并"让
汇合点自己浮现。对结构化的 `if/else`，效果和 IPDOM 一样；对 `ret`
和早退，它不需要任何额外分析。

## 设计

### 数据结构

```python
class Warp:
    def __init__(self, lanes, pc, ur0=0):
        self.lanes = lanes
        self.frames = [(pc, (1 << len(lanes)) - 1)]   # [(pc, mask), ...]
        self.pc = pc
        self.mask = self.frames[0][1]
        self.ur = [0] * 8
        self.ur[0] = ur0
```

`warp.pc` / `warp.mask` 是**当前正在执行的那一帧**的副本；`frames`
是真相。`active_lanes()` 用 `self.mask` 取出本帧的 lane 下标。

### 归一化：合并就是重汇聚

```python
def _normalize(self):
    merged = {}
    for pc, mask in self.frames:
        if mask:
            merged[pc] = merged.get(pc, 0) | mask
    self.frames = sorted(merged.items())
```

- 同一个 pc 的帧取掩码并集——这是唯一的重汇聚动作；
- 空掩码帧直接丢弃；
- 排序后第一项就是最小 PC。

### 调度：最小 PC 先跑

```python
def select(self):
    self._normalize()
    if not self.frames:
        return False
    self.pc, self.mask = self.frames[0]
    return True
```

`_run_warp` 每步先 `select()`，再执行一条指令。`select()` 返回 False
表示这个 warp 的所有 lane 都 `exit` 了。

### 分支：拆帧

```python
taken = 0
for i in warp.active_lanes():
    if not has_pred or lane.read_p(pred):
        taken |= 1 << i
not_taken = warp.mask & ~taken
if taken and not_taken:
    if self.strict_uniform:
        raise SimError("non-uniform branch ...")
    warp.set_current([(pc + off, taken), (pc + 1, not_taken)])
elif taken:
    warp.jump(pc + off)
else:
    warp.jump(pc + 1)
```

三路：全 taken、全 not-taken、真分歧。前两种就是第 12 章的行为，
第三种拆成两帧。注意偏移的单位是**字**，不是字节。

### `ret`：按 `lr` 分组

```python
groups = {}
for i in warp.active_lanes():
    target = lane.read_r(31)      # r31 = lr，每 lane 一份
    groups[target] = groups.get(target, 0) | (1 << i)
warp.split(groups)
```

不同 lane 可能从同一条 `ret` 返回不同位置——递归和"调用深度不一致"
都会出现。按 `lr` 分组是唯一正确的做法；用一个"warp 返回地址"会静默
跑错。

### `exit`：只结束当前帧

```python
if funct == 0:              # EXIT
    warp.set_current([])    # 当前帧的 lane 全部结束
    continue
```

第 12 章里 `exit` 等于"整个 warp 结束"。分歧之后这只对**当前帧**
成立：另一支的 lane 还欠着几条指令。

### `activemask` 的语义

```python
lane.write_r(rd, self._special(warp, lane, name, warp.mask))
```

`sr.activemask` 返回**当前帧**的掩码。于是：

- 在 `lowarm` 里读，得到 `0x0f`；
- 在 `higharm` 里读，得到 `0xf0`；
- 在 `join` 合并之后读，得到 `0xff`。

这正是 CUDA `__activemask()` 的语义，也是"重汇聚发生了"的可观测证据。

### `--strict-uniform`

```python
if self.strict_uniform:
    raise SimError("non-uniform branch at pc=%d; --strict-uniform is the "
                   "chapter 12 model" % pc)
```

这条开关存在的理由：第 12 章的门禁和文档说的是"一个 warp 一个 PC"，
那个模型必须仍然可复现。默认行为向前走，回归测试用开关把旧语义要
回来，比保留两份执行器干净得多。

### 复杂度与终止性

设帧数 `F`、lane 数 `L`。每步 `_normalize` 是 `O(F log F)`，`F <= L`
（帧掩码互不相交），所以最坏 `O(L log L)`；教学机的 `L = 8`。

终止性由两件事保证：

1. 每条指令都让当前帧的 pc 前进、跳转或消失，不会原地不动；
2. 帧数不会无限增长：`split` 只切分现有掩码。

真正会打爆的是程序自己的死循环——那由 `--max-steps` 兜底。

## 实现

### 模拟器

改动集中在 `tools/gpumin_sim.py` 的 `Warp` 类（`frames` / `_normalize`
/ `select` / `set_current` / `split`）和 `_run_warp` 的 `BRA`、`CALL`、
`RET`、`EXIT` 四个分支。

调试时打开 `--trace`，每步会打印：

```text
warp b0 w0 pc=12 mask=0f frames=2 word=...
```

`frames=2` 表示当前 warp 有两帧在飞，`mask=0f` 是正在执行的这一帧。

### 编译器：三个被分歧测试暴露的缺口

写 `divergence.ll` 时 `llc` 直接崩了三次，每一个都是真实缺口：

**1. `trunc i32 -> i1` 没有选择规则。**

```text
LLVM ERROR: Cannot select: t26: ch = br_cc ..., seteq:ch, t23, Constant:i1<0>,
  t23: i1 = truncate t4
```

`i1` 在本后端是合法类型（谓词寄存器），所以类型合法化不会动它；
但 GM32 没有 bitfield extract，也没有立即数 `and`。把它写成 DAG
combine 会踩到死循环（`(x & 1) != 0` 会被 `SimplifySetCC` 规范化回
`trunc`），所以最终放在 ISel 里显式展开：

```text
movi rt, 1 ; and rd, rs, rt ; movi rz, 0 ; setp.ne pd, rd, rz
```

**2. i1 作为分支条件。**

DAG combiner 会把 `br i1 (trunc x)` 改写成
`br_cc (trunc x), true, setne`，再被合法化成 `xor %p, true`。谓词
没有 NOT 指令，所以 ISel 里把 `xor i1, true` 展开成
`selp 0/1 + setp.eq 0`；其他 i1 布尔运算（`and/or/xor`）同样走
"升到 32 位算完再降回谓词"。

**3. `sr.activemask` 必须是 effectful 的。**

这条最重要：把 `activemask` 标成 `IntrNoMem` 的纯读时，SelectionDAG
的 CSE 会把三个分支里的三次读合并成一个，并把它调度到 entry——于是
分支里读到的永远是分歧前的 `0xff`。修法是两层：

```tablegen
def int_gpumin_activemask : Intrinsic<[llvm_i32_ty], [],
                                      [IntrHasSideEffects, IntrWillReturn]>;

let hasSideEffects = 1 in
defm SR_ACTIVEMASK : GPUMinSR<0x008, "sr.activemask">;
```

intrinsic 带 side effect 让 DAG 不合并，指令带 side effect 让机器层
CSE 不动它。门禁里那条 `grep -c sr.activemask == 3` 就是这条的性质锁。

### 运行

```sh
llc -march=gpumin -O1 -filetype=obj scripts/e2e/divergence.ll -o /tmp/d.o
python3 tools/gpumin_sim.py /tmp/d.o --kernel diverge --threads 8 \
        --arg 0x4000 --dump 0x4000=32
```

每个 lane 的四字记录是 `[tid, 分支内 mask, 分支值, join 后 mask]`：

```text
0 15 0 255     1 15 2 255     2 15 4 255     3 15 6 255
4 240 104 255  5 240 105 255  6 240 106 255  7 240 107 255
```

`15 = 0x0f`、`240 = 0xf0` 是两半 warp，`255 = 0xff` 是重汇聚。

`triangle` 让每个 lane 跑不同圈数（`sum_to(tid)`）：

```text
0 0 1 3 6 10 15 21
```

lane 0 不进循环、lane 7 跑 7 圈，出口只在最后一支到达时才合并。

## 验证

```text
PASS 1/4: the model names the policy and activemask is read per region
PASS 2/4: half the warp takes each arm and the join restores 0xff
PASS 3/4: loop exits at different iterations and a nested diamond agree
PASS 4/4: --strict-uniform rejects divergence and still runs uniform code
```

四组分别覆盖：

1. `simt-model.json` 里的 `divergence.policy = "min-pc-frame-merge"`，
   以及汇编里三次 `sr.activemask` 都还在（没有被 CSE 掉）；
2. 一个 warp 一分为二、两支各自算值、join 后 mask 回到 `0xff`；
3. 每 lane 不同 trip count 的循环，以及循环体内的嵌套 diamond；
4. `--strict-uniform` 对同一个程序报错，但对 uniform kernel 仍然放行。

总验收仍是 8 项门禁，第 6 项现在已经包含第 12、13、14 三章：

```text
Gates: 7 passed, 0 failed, 1 skipped
```

第 7 项要等第 18 章的调度模型。

## 常见坑

1. **不合并同 PC 的帧**。帧会指数增长，而且 `join` 后再读
   `activemask` 得到的是"交错执行"的假象。`_normalize` 必须每步跑。
2. **用 FIFO 或最大 PC 调度**。FIFO 会让"先到汇合点"的一支先跑，
   重汇聚变成"看谁快"；最大 PC 会先跑汇合点，等于破坏等待语义。
   最小 PC 是让结构化 if/else 自然汇合的关键。
3. **`ret` 用一个返回地址**。每 lane 各有 `lr`；分组拆帧是唯一正确
   的写法。
4. **`exit` 结束整个 warp**。分歧中提前退出的 lane 会把另一支也带走，
   表现为结果少一半、join mask 永远到不了 `0xff`。
5. **把 `activemask` 当纯函数**。见上文：DAG CSE 会合并三次读，得到
   分歧前的 mask。它不是"读状态"，是"读当前执行上下文"。
6. **忘了 `--strict-uniform`**。第 12 章的门禁复现旧语义时要带上它，
   否则那条 `non-uniform branch` 断言永远不触发。
7. **分支偏移按字节算**。`BRA` 的 16 位字段是字数；写字节数会落到
   指令中间。
8. **只测结构化 if**。一定要测循环：不同 trip count 的汇合点才真正
   考验"最小 PC + 合并"。
9. **把 `warp.mask` 当成"warp 的掩码"**。它是**当前帧**的掩码；
   整 warp 的活跃集合是 `frames` 里所有 mask 的并集。
10. **以为动态重汇聚等于慢**。帧数上限是 lane 数（8），每步多一次
    排序；换来的是 `ret`/早退/不规则 CFG 全都免费支持。

## 练习

1. 把 `select()` 改成"取最大 PC"，跑 `triangle`，解释结果为什么错。
2. 给 `--trace` 增加 `frames` 的完整列印，统计每个程序的最大帧数。
3. 实现一个静态 IPDOM 版本（只看结构化 if/else），在 `divergence.ll`
   上和本章实现对比输出；再让它跑 `fib`。
4. 把 `divergence.policy` 从 JSON 读进模拟器，支持
   `min-pc` / `fifo` / `strict` 三种策略，并让 `--strict-uniform`
   变成它的别名。
5. 写一个 8 层嵌套 if 的生成器，验证帧数始终 `<= 8`。
6. 让一半 lane 在一个循环里 `exit`，另一半继续；验证 join 后的 mask
   和后续读取。
7. 思考：barrier（第 17 章）要求"整 block 每个 lane 都到达"，这和
   帧集合是什么关系？为什么 barrier 不能进入帧？

## 下一章

第 15 章把前面所有零件拼起来做 **SIMT 验收**：vecadd、SAXPY 和
block 内 reduction 三个程序，跨 block/warp/lane 三个层次检查结果，
并输出一份执行统计（指令数、lane 指令数、帧数峰值），作为后面
shared memory（16）、barrier（17）、调度与 occupancy（18）的基线。
