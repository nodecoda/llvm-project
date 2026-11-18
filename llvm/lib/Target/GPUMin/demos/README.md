# GPUMin 演示：一条命令看后端做了什么

这组例子不是新的门禁，而是**可运行的说明书**：每个 `.ll` 文件只突出一个
后端特性，`run-demos.sh` 依次用 `llc` 编译、打印关键目标指令、在
`tools/gpumin_sim.py` 上运行，并把输出与硬编码的期望值对拍。

```sh
# 全部 7 个例子
demos/run-demos.sh <llvm-bin-dir>

# 只跑其中几个（编号就是文件名前缀）
demos/run-demos.sh <llvm-bin-dir> 03 06
```

`<llvm-bin-dir>` 是包含 `llc` 的构建目录，例如
`/home/ubuntu/llvm/builds/<task>/bin`。需要 `llc`、`python3`；模拟器从
ELF 对象里自己解码，不依赖 `llvm-objdump`。

每个例子打印三段：

1. `--- target instructions matching /.../ ---`：从 `.s` 里摘出的关键
   指令，说明该特性在汇编层长什么样；
2. `--- output (stdout) ---`：模拟器 dump 出的全局内存，脚本会与期望值
   逐字比对；
3. `--- statistics (stderr) ---`：发射槽、访存、分歧、barrier 等计数，
   脚本对其中少数**语义不变式**做断言（例如 uniform 分支必须
   `splits=0`），其余数字打印出来供观察。

## 例子一览

| # | 文件 | kernel | 展示的特性 | 启动参数 | 期望输出 | 对应章节 |
| --- | --- | --- | --- | --- | --- | --- |
| 01 | `01-special-registers.ll` | `ids` | 8 个 `sr.*` 各有独立指令 | 2 blocks × 16 threads | 32 条 × 8 字 | 09 |
| 02 | `02-alu-and-constants.ll` | `alu` | 32 位常量、`sar`、`and/xor/or` | 1 × 8 | 8 个 i32 | 04、08 |
| 03 | `03-control-flow.ll` | `control` | 循环 + uniform if/else | 1 × 8 | `16..23` | 05 |
| 04 | `04-memory-widths.ll` | `widths` | `b8/u8/s8/b16/u16/s16` | 1 × 8 | 4 组 × 8 字 | 06、08 |
| 05 | `05-device-calls.ll` | `calls` | `call`/`ret`、`r30`/`r31`、私有栈 | 1 × 8 | `1 13 … 85` | 07 |
| 06 | `06-divergence.ll` | `divergent` | 真分歧、frame stack | 1 × 8 | 三角数 | 11、14 |
| 07 | `07-block-reduction.ll` | `block_sum` | 共享内存、`bar.sync`、block 作用域 | 2 blocks × 32 threads | `528 528` | 16、17 |

约定：输入从全局地址 `0x0` 开始，输出从 `0x200` 起；04 用
`0x200/0x300/0x400`，07 用 `0x2000`。`--fill ADDR=START,STEP,COUNT` 生成
输入，`--dump ADDR=COUNT` 以 **32 位字**为单位输出结果。

## 每个例子看什么

### 01 特殊寄存器

```sh
llc -march=gpumin -O1 -filetype=obj 01-special-registers.ll -o /tmp/01.o
python3 ../tools/gpumin_sim.py /tmp/01.o --kernel ids \
  --blocks 2 --threads 16 --arg 0x0 --dump 0x0=256 --stats
```

每 lane 写 8 个 32 位字：`tid, laneid, warpid, blockid, ntid, nwarps,
activemask, shared`。看汇编里 `sr.tid`、`sr.laneid` … `sr.shared` 是 8 条
不同的指令，而不是一个通用“读特殊寄存器”的接口；再看输出中
`laneid = tid % 8`、`warpid = tid / 8`、`activemask = 0xff`，以及两个 block
的 `blockid` 分别为 0 和 1。

### 02 ALU 与常量

```sh
python3 ../tools/gpumin_sim.py /tmp/02.o --kernel alu --blocks 1 --threads 8 \
  --arg 0x0 --arg 0x200 --fill 0x0=-1000000,123456789,8 \
  --dump 0x200=8 --stats
```

三个观察点：

- `0x12345678` 放不进 16 位立即数，选择器展开为
  `movi hi; shl 16; movi lo; shl; shr; or`——属于第 08 章常量表的
  “其他 32 位”一行；
- `65535` 虽然是无符号 16 位，但 `movi` 会符号扩展，所以也要
  `shl 16; shr 16` 清掉高位；
- `ashr` 选成 `sar`，与逻辑右移 `shr` 是不同的指令。

### 03 uniform 控制流

条件只依赖常量，所有 lane 同时走同一分支。`--stats` 显示
`splits=0 frames_max=1`；把它和 06 对比，就是“分支”与“分歧”的区别。

### 04 访存宽度

同一份输入按 `i8`/`i16` 截断写出，再分别用 `u8`、`s8`、`u16`、`s16`
读回并扩展成 32 位。输出四组：`255` 与 `-1` 是同一字节的两种解释，
`65535` 与 `-1` 是同一半字的两种解释。汇编里能看到
`st.global.b8/b16` 和 `ld.global.u8/s8/u16/s16`。

### 05 设备函数与栈

kernel `calls` 调用 `@quad`，`@quad` 又两次调用 `@twice`。看 `quad` 的
序言：`addi r30, r30, -16` 开帧，`st.local.b32 r31, 12(r30)` 保存返回地址，
两次 `call twice` 后恢复 `r31` 和 `r30`。`r30` 是栈指针，`r31` 是链接
寄存器；没有寄存器分配器 spill 时，`call` 本身不改栈。

### 06 真分歧

循环次数来自每个 lane 自己的输入，lane 在不同迭代退出。模拟器用 frame
stack 保存落后的 lane，在汇合块合并。`--stats` 里 `splits=7`、
`frames_max=2`；`lane_instrs=245` 远小于“最坏 lane 指令数 × 8”，说明
落后 lane 被挂起而不是空转。

### 07 共享内存、barrier 与 block 作用域

两个 block 各 32 lane。第一段每个 lane 把自己的输入写进本 block 的
`addrspace(2)` 窗口，`bar.sync` 之后由 lane 0 求和。输出 `528 528`：
block 0 的和是 1..32，block 1 的和是 33..64，**两个窗口互不可见**。
统计里 `barriers=2`（每 block 一次）、`glb_ld=64`、`shm_st=64`、
`glb_st=2`，清楚看到 global → shared → 一个标量结果 的流量收缩。

## 与门禁的关系

`scripts/gpumin-verify-all.sh` 的第 8 项会锁定 lit 文件数、章节数、
门禁数等静态计数。demos 是教学材料，不是章节，因此**不进入这 8 项计数**，
避免为了加一个演示而改 `static-counts.txt`。demos 自身是自检的：任何
输出或不变式不符都会以非零状态退出，可以直接放进 CI 的额外一步。

## 新增一个演示

1. 复制一个现有 `.ll`，只保留一个主题，文件命名为 `NN-<topic>.ll`；
2. 在 `run-demos.sh` 末尾加一个 `run_demo` 调用：标题、kernel 名、
   汇编 grep、期望输出、模拟器参数；
3. 如果这个特性有语义不变式（例如“必须不分裂”），再加一条
   `check ... "$(stat NN key)"`；
4. 运行 `run-demos.sh <bin>` 确认全绿，并把例子补进上面的表格。
