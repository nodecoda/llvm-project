# 第 13 章：kernel 启动——launch descriptor 与 `sr.args`

## 目标

第 12 章的模拟器已经能跑 kernel，但启动方式还是"把参数塞进 `r4-r11`"。
这只是为了让前面的章节能跑起来的**临时 ABI**：它最多 8 个参数，没有
grid/block 形状，也无法表达"参数块在内存里"这种真实 GPU 的做法。

本章把启动状态变成一个显式的、可复现的输入：

1. 新增特殊寄存器 `sr.args`（SYS funct 9）：返回 launch 参数块地址；
2. launcher 把该地址写进 uniform 寄存器 `ur0`，所有 lane 读到同一个值；
3. 模拟器新增 `--launch FILE`：用 JSON 描述 grid、block、参数块和
   内存填充/转储；
4. kernel 的 ABI 变成"无形式参数，用 `sr.args` 取参数块指针"；
5. 用 2 block × 8 thread 的 vecadd 验收；
6. 缺 launch、block 非法、参数块越界都必须报错。

退出条件（可执行）：

```sh
llvm/lib/Target/GPUMin/scripts/check-ch13.sh <llvm-bin-dir>
```

## 背景

### 为什么 `r4-r11` 不够

第 03–12 章的 kernel 都是这样启动的：

```text
--arg 0x0 --arg 0x2000 --arg 0x4000 --arg 32
        r4        r5        r6        r7
```

它能跑，但有三个问题：

1. **参数个数硬上限**：8 个寄存器，超过就没有地方放；
2. **没有 launch 形状**：grid/block 是命令行选项，不是 kernel 可见的状态；
3. **和真实 ABI 不像**：GPU 的 kernel 参数通常在一个常量/全局参数块里，
   由驱动在 launch 时准备好，kernel 只拿到一个指针。

真实 GPU 还会把 grid/block 维度、参数块大小、shared memory 大小等打包成
一个 launch descriptor。本章做一个教学版：JSON 描述，Python launcher
负责把参数块写进 global memory，再把指针交给 `ur0`。

### 为什么是 `ur0`，不是 `r0`

第 01 章把 `r0` 固定为零寄存器。它是一个**每 lane** 的寄存器，而且写入
无效；拿它存 launch 状态会直接破坏地址算术。

`ur0-ur7` 是**每 warp 一份**的 uniform 寄存器。同一 warp 的所有 lane
看到同一个 `ur0`，正好符合"参数块地址对整次 launch 是常量"的语义。
第 11 章的模型已经写明：

```text
ur 只作为 kernel 参数/块索引的载体（第 13 章）
```

本章兑现这句话：`ur0 = args 指针`。硬件上它由 launch 单元写入；
模拟器里由 `apply_launch()` 写入。

### 为什么 `sr.args` 是一条特殊寄存器读

编译器需要把 `ur0` 变成一个普通的 `i32` 值，才能做 GEP、load 和地址
算术。第 09 章已经有"一个 selector 一条 SYS 指令"的机制：

```text
[31:26] 0x12 | [25:21] rd | [20:16] 0 | [15:11] 0 | [10:0] funct
```

把 `funct = 9` 分配给 `sr.args`，编码、解码、打印、DAG 匹配全部复用
现有生成器；IR 侧只需要一个 intrinsic：

```llvm
%args = call i32 @llvm.gpumin.args()
```

它返回的是**字节地址**，不是指针类型。GPUMin 的指针是 32 位，IR 里用
`inttoptr` 转成 `ptr addrspace(1)` 后再做 GEP。

### 1D grid 与 `gid`

本章只做 1D：

```text
grid  = 2
block = 8
gid   = blockid * 8 + tid
```

注意 `tid` 是 **block 内**线程号，不是全局线程号。`blockid` 由 launcher
写进每条 lane 的状态，`tid/laneid/warpid` 由第 11 章的模型给出。

`launch.ll` 里写的是 `gid = (blockid << 3) + tid`，因为 MVP 的 ALU 没有
乘法。把 block 固定为 8 是本章的契约；要写通用 kernel，需要 `blockDim`
乘法，这会留到后续扩展。第 20 章的 matmul 也用固定 tile 尺寸。

## 设计

### launch descriptor

`--launch FILE` 读取一个 JSON：

```json
{
  "grid": 2,
  "block": 8,
  "args_addr": 256,
  "args": [0, 4096, 8192],
  "fill": [
    { "addr": 0, "start": 0, "step": 1, "count": 16 },
    { "addr": 4096, "start": 0, "step": 3, "count": 16 }
  ],
  "dump": [
    { "addr": 8192, "count": 16 }
  ]
}
```

| 字段 | 含义 | 约束 |
| --- | --- | --- |
| `grid` | block 数 | ≥ 1 |
| `block` | 每 block 线程数 | 8 的倍数，8..32 |
| `args_addr` | 参数块字节地址 | 4 字节对齐，默认 `0x100` |
| `args` | 参数块里的 32 位字 | 必须能放进 global memory |
| `fill` | 运行前填充 global 的区间 | `addr/start/step/count` |
| `dump` | 运行后转储的区间 | `addr/count` |

模拟器做四件事：

1. 用 `grid/block` 设置 block 数、每 block 线程数、warp 数；
2. 把 `args` 写到 `args_addr`；
3. 每条 warp 的 `ur0` 设为 `args_addr`；
4. 执行 kernel，最后按 `dump` 打印。

`--arg` 仍然保留给第 12 章的旧样例；两者**不能同时使用**，因为它们
描述的是两种不同的 kernel ABI。

### kernel 侧

新的 kernel 没有形式参数：

```llvm
define void @vecadd_launch() #0 {
  %args = call i32 @llvm.gpumin.args()
  %ap = inttoptr i32 %args to ptr addrspace(1)
  %a = load i32, ptr addrspace(1) %ap
  ...
}
attributes #0 = { "gpumin_kernel" }
```

参数块的布局由 kernel 和 launcher 约定：

```text
args[0] = a   指针（字节地址）
args[1] = b   指针
args[2] = out 指针
```

真实 ABI 会把参数块放在 constant 空间；本章放在 global，因为模拟器的
constant 空间还没有写入路径，而 global 已经能 load。这个取舍写进
第 16 章的改进列表。

### 编译器：`sr.args` 的选择

`IntrinsicsGPUMin.td` 增加：

```tablegen
def int_gpumin_args : Intrinsic<[llvm_i32_ty], [],
                                [IntrNoMem, IntrSpeculatable,
                                 IntrWillReturn]>;
```

`GPUMinInstrInfo.td` 增加：

```tablegen
defm SR_ARGS : GPUMinSR<0x009, "sr.args">;
def : Pat<(int_gpumin_args), (SR_ARGS)>;
```

没有新的格式、没有手写 parser：SYS 的 R1 格式已经存在，TableGen 从
`Inst{}` 描述同时生成 encoder 和 decoder。

### 模拟器：launch 状态

`Simulator.apply_launch(desc)`：

```text
validate grid/block
args_addr = desc.args_addr or 0x100
write desc.args at args_addr
warps[i].ur[0] = args_addr
```

`SYS funct 9` 的执行：

```text
if args_addr is None: error("sr.args read without a launch descriptor")
rd = warp.ur[0]
```

这条错误很重要：如果 kernel 使用新 ABI，却忘了给 `--launch`，它必须
立刻失败，而不是读到 `ur0 = 0` 然后访问地址 0。

### 启动形状与生命周期

```text
for blockid in 0 .. grid-1:
    reset shared memory
    for warp in 0 .. block/8-1:
        lanes get tid = warp*8 + laneid, ntid = block, nwarps = block/8
        ur0 = args_addr
        run the warp until exit
```

kernel 的 `ret void` 仍然降成 `exit`；`exit` 清掉 lane 的 active mask，
所有 warp 结束后 block 结束。第 17 章的 barrier 会把"warp 串行"换成
block 级调度。

## 实现

### 编码

```text
sr.args r1  ->  0x48200009  ->  [0x09,0x00,0x20,0x48]
```

`funct = 9`，`rd = 1`，其余字段为 0。反汇编器用同一个 `funct` 表把它
还原成 `sr.args r1`。

### CodeGen

`launch.ll` 在 `-O1` 下生成：

```asm
vecadd_launch:
	movi r1, 3
	sr.blockid r2
	shl r1, r2, r1        # blockid << 3
	sr.tid r2
	add r1, r1, r2        # gid
	movi r2, 2
	shl r1, r1, r2        # gid * 4 = byte offset
	sr.args r2            # r2 = args pointer
	ld.global.b32 r3, 4(r2)   # b
	add r3, r3, r1
	ld.global.b32 r3, 0(r3)   # b[gid]
	ld.global.b32 r4, 0(r2)   # a
	add r4, r4, r1
	ld.global.b32 r4, 0(r4)   # a[gid]
	add r3, r4, r3
	ld.global.b32 r2, 8(r2)   # out
	add r1, r2, r1
	st.global.b32 r3, 0(r1)   # out[gid]
	exit
```

这段汇编同时证明了四件事：`sr.args` 选择成功、`sr.blockid/sr.tid`
参与地址计算、参数块通过普通 global load 读取、kernel 以 `exit` 结束。

### 运行

```sh
BIN=/home/ubuntu/llvm/builds/pr-ctpop/bin
cd llvm/lib/Target/GPUMin
$BIN/llc -march=gpumin -O1 -filetype=obj scripts/e2e/launch.ll -o /tmp/launch.o
python3 tools/gpumin_sim.py /tmp/launch.o --kernel vecadd_launch \
  --launch scripts/e2e/launch-vecadd.json
# 0 4 8 12 16 20 24 28 32 36 40 44 48 52 56 60
```

输出有 16 个元素，说明两个 block 各写了 8 个；如果 launcher 只跑一个
block，后 8 个位置会保持 0。门禁用这个性质验证 `grid` 真的生效。

## 验证

```text
PASS 1/4: sr.args is funct 9 in the model, MC layer and ISel
PASS 2/4: 2 blocks x 8 threads run the argument-block vecadd
PASS 3/4: descriptor values are honoured and the legacy ABI still runs
PASS 4/4: missing and malformed launch state fail loudly
```

四组分别覆盖：

1. 模型 JSON、`llvm-mc` 编码、`llc` 选择三方一致；
2. 默认 descriptor 的 2×8 vecadd；
3. 换一组 fill 值后结果随之改变（证明不是写死的），以及 grid=1 只写
   前 8 个；同时回归第 12 章的 `--arg` ABI；
4. 无 launch、block=12、grid=0、参数块越界四种错误。

总验收在这一章之后仍是 8 项门禁，其中第 6 项现在同时跑第 12、13 章：

```text
Gates: 7 passed, 0 failed, 1 skipped
```

第 7 项要等第 18 章的调度模型。

## 常见坑

1. **把 `args_addr` 当成指针类型**。它是字节地址；kernel 里要
   `inttoptr` 再 GEP。
2. **参数块和 fill 区重叠**。模拟器不会替你检查；overlap 会让参数被
   数据覆盖。选地址时留出间隔。
3. **`block` 不是 8 的倍数**。教学机的 warp 是 8 lane，半个 warp 没有
   定义；launcher 在入口拒绝。
4. **`gid` 用了固定的 32**。本章 block=8，所以是 `<< 3`；如果 launch
   改成 16 或 32，kernel 必须同步改，或者等 ISA 支持乘法。
5. **忘了 `--launch`**。`sr.args` 会报 "without a launch descriptor"；
   不要把它改成返回 0，否则会静默访问地址 0。
6. **`--launch` 和 `--arg` 混用**。前者是参数块 ABI，后者是寄存器
   ABI；混用会让 kernel 读到两套不同的参数。
7. **认为 `blockid` 是全局线程号**。它是 block 索引；全局号要自己算
   `blockid * block + tid`。
8. **constant 空间**。本章参数块放在 global；如果改到 constant，要
   先让 launcher 有写入 constant 的路径。
9. **2D/3D launch**。模型里 `grid_dims = 1`；传别的值会被拒绝。
10. **把 `sr.args` 当每 lane 值**。它来自 `ur0`，一个 warp 内必须一致；
    第 14 章的分歧不会改变它。

## 练习

1. 把 `args[3]` 加成 `n`，在 kernel 里做边界检查；解释为什么这个
   检查在 2×8、n=16 时是 uniform，而在 n=12 时会触发第 14 章的分歧。
2. 给 launch JSON 加 `"shared": 1024`，让模拟器按 block 分配 shared
   内存；这是第 16 章的入口。
3. 写一个 kernel，把 `args[0..3]` 原样写到 out，验证参数块布局；
   再用不同的 `args_addr` 跑一次。
4. 把 `launch.ll` 的 block 从 8 改成 16，同步改 `gid` 的移位；讨论
   为什么"通用 kernel"需要 `blockDim` 乘法。
5. 给 launcher 加 `--dry-run`，只打印 grid/block/args 布局而不执行；
   思考真实驱动在 launch 前会验证哪些字段。
6. 把第 12 章的旧 kernel 迁移到 `sr.args` ABI，保留 `--arg` 版本作为
   回归测试，比较两种 ABI 的汇编差异。

## 下一章

第 14 章加入 **SIMT stack**：同一 warp 内的 lane 可以走不同分支，在
immediate post-dominator 重新汇合。第 12 章那条
`non-uniform branch` 报错会被真正的分歧执行替换；`sr.args` 和 launch
状态保持不变，只是分支后的 lane 集合不再是"整 warp 一起走"。
