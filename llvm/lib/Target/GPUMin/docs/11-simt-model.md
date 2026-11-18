# 第 11 章：SIMT 模型——lane、warp、active mask 与 uniform

## 目标

第 10 章把静态后端冻结了。本章不加指令，也不改编码，而是把
"一个 GM32 程序到底怎么执行"写成一份**可执行模型**：

1. `scripts/simt-model.json` 是唯一的数字来源：warp 宽度、每 block 的
   warp 数、特殊寄存器 `funct`、active mask 位宽、地址空间；
2. `tools/gpumin_model.py` 读取这份 JSON，提供 `laneid`/`warpid`/`tid`/
   `gid`/`activemask` 和七种特殊寄存器的取值；
3. 本章文档给出规范性语义：状态所有权、执行规则、分歧规则、内存空间、
   uniform 约束；
4. 第 09 章的 `laneid`/`warpid` lowering 已经就位，本章把它和模型对齐；
5. 第 12 章的 Python 模拟器直接 `import` 这个模型，不再复制一套数字。

退出条件（可执行）：

```sh
llvm/lib/Target/GPUMin/scripts/check-ch11.sh <llvm-bin-dir>
```

## 背景

### 单线程标量 IR，硬件把线程组成 warp

LLVM IR 看到的是**一条 lane 的视角**：

```llvm
%tid = call i32 @llvm.gpumin.tid()
%p = getelementptr i32, ptr addrspace(1) %a, i32 %tid
%x = load i32, ptr addrspace(1) %p
```

硬件（或模拟器）把 8 条执行同一段程序的 lane 组成一个 warp，让它们在
同一个 PC 上取指。这样：

- 类型合法化、ISel、RA、调度都不需要懂 warp；
- lane/warp/block 身份通过特殊寄存器暴露；
- 分歧由 active mask 处理，而不是把 IR 变成 `<8 x i32>`。

如果把 warp 建模成向量类型，控制流分歧就无法表达，ABI、shared memory
和 barrier 的语义也会跟着变形。这就是整套设计坚持"标量 IR"的原因。

### 层级

```text
grid
  └── block        32 threads = 4 warps × 8 lanes
        └── warp   8 lanes，共享一个 PC 和一个 active mask
              └── lane   一条执行流，有自己的 r0-r31 和 p0-p7
```

教学配置固定为：

```text
warp_size = 8
warps_per_block = 4
threads_per_block = 32
```

这些数字不是"随便选的"：它们让一个 CUDA block（`blockDim.x = 32`）
正好容纳 4 个教学 warp，附录 A.4 的 GPU 解释器可以直接一对一映射。

### 三个恒等式

```text
tid = warpid * 8 + laneid          // block 内线程号，0..31
gid = blockid * 32 + tid           // 全局线程号
nwarps = 4
ntid = 32
```

`laneid` 是 warp 内的 0..7，`warpid` 是 block 内的 0..3，`tid` 是
block 内的 0..31，`gid` 才是整个 grid 的线程号。`tid` **不是**全局
线程号；这是 CUDA `threadIdx.x` 的语义。

### active mask

每个 warp 有一个 8 位 active mask：bit i 为 1 表示 lane i 当前活跃。
全活跃时：

```text
lane:   7 6 5 4 3 2 1 0
bit:    1 1 1 1 1 1 1 1   = 0xff
```

`activemask` 特殊寄存器返回这个 32 位整数（高 24 位为 0）。它不是
谓词寄存器：`p0-p7` 是每条 lane 各有一份的 1-bit 状态；active mask 是
整个 warp 共享的位图。

### 为什么模型要独立于编译器

同一个数字会出现在至少四个地方：文档、TableGen 编码、测试、
模拟器。如果每处各写一遍，迟早会漂移：文档说 8，模拟器用 32，
`laneid` 的测试却按 0..31 写。第 11 章把数字收进
`scripts/simt-model.json`，文档和门禁都从它派生；第 12 章的模拟器
`import` 同一个 Python 模块。

## 设计

### 状态所有权

| 状态 | 作用域 | 数量 | 说明 |
| --- | --- | --- | --- |
| PC | warp | 1 | 每个 warp 一个 PC |
| active mask | warp | 1 | 8 位位图，bit i = lane i |
| `r0-r31` | lane | 32 | `r0` 恒 0；`r30=sp`；`r31=lr` |
| `p0-p7` | lane | 8 | 谓词文件 |
| `ur0-ur7` | warp | 8 | uniform 寄存器 |
| shared memory | block | 一块 | `addrspace(2)` |
| global memory | grid | 一块 | `addrspace(1)` |
| private memory | lane | 每 lane 一块 | `addrspace(3)`，栈 |
| constant memory | grid | 一块 | `addrspace(4)`，只读 |

"每个 warp 一个 PC"是关键：同一 warp 的 8 条 lane 在正常情况下执行
同一条指令；分歧时由模拟器/硬件用 mask 和 SIMT stack 让不同子集
走不同路径。

### 执行规则

1. **启动**：1D grid；每个 block 有 32 条 lane（4 warp × 8 lane）；
   每条 lane 的 PC 指向 kernel 入口，warp 的 active mask 初始为 `0xff`。
2. **取指/执行**：每个 warp 每个周期取一条 GM32 指令；mask 为 0 的
   lane 不产生副作用，寄存器和内存都不变。
3. **谓词**：`@p` 指令只让 `p=1` 的 lane 生效；这与"控制流分歧"不同，
   谓词只抑制一条指令。
4. **EXIT**：执行 `exit` 的 lane 把自己的 mask 位清零；mask 变成 0 时
   warp 结束；block 的所有 warp 都结束后，block 结束。
5. **分支**：`bra` 改变 warp 的 PC；`bra pN, target` 在 MVP 的结构化
   控制流里由前端保证同一 warp 的 lane 走同一路径。第 14 章引入
   SIMT stack 后，才允许同一 warp 内不同 lane 走不同分支并在
   immediate post-dominator 重新汇合。
6. **barrier**：第 17 章的 `bar.sync` 是 block 级、必须 uniform；一个
   block 的所有 lane 都必须到达，提前 `exit` 的 lane 会造成死锁。
7. **内存**：`global` 在所有 block 间共享；`shared` 每个 block 一份；
   `private` 每条 lane 一份；`constant` 只读。MVP 不定义弱内存序，
   程序必须无竞争；barrier 是唯一的跨 lane 同步点。
8. **uniform**：`ur0-ur7` 每个 warp 一份，写入的值必须与 lane 无关。
   kernel 参数指针、`blockid` 这类值适合放 uniform 文件；`laneid`
   这类每 lane 不同的值不能写进 `ur`。MVP 还没有 uniform ALU，
   `ur` 只作为 kernel 参数/块索引的载体（第 13 章）。

### 分歧模型（MVP 与第 14 章）

MVP 只承诺**结构化** if/else：

```text
if (cond) { A } else { B }
```

在机器层，这仍然是 `bra p0, else` + 两个分支块。模拟器可以先用
"整条 warp 一起走一条路径"的保守模型：只要程序是结构化的，路径
选择对每条 lane 一致，结果就是对的。

第 14 章要支持任意 CFG 分歧：

```text
if (cond) { A } else { B }
A 和 B 内部又各有分支
```

这时需要 SIMT stack：每个栈项记录 `(PC, active mask, reconvergence PC)`；
遇到分歧时把当前 mask 拆成两个子 mask，先执行一个，再执行另一个，
到 reconvergence 点合并。第 11 章先把"mask 是什么、谁拥有它"冻结，
第 14 章再实现调度算法。

### 与 CUDA 的映射

| GM32 | CUDA |
| --- | --- |
| 1 lane | 1 CUDA thread |
| 1 warp = 8 lane | CUDA warp 的 32 thread 中打包 4 个教学 warp |
| 1 block = 32 lane | CUDA block，`blockDim.x = 32` |
| `laneid` | `threadIdx.x % 8` |
| `warpid` | `threadIdx.x / 8` |
| `tid` | `threadIdx.x` |
| `blockid` | `blockIdx.x` |
| `ntid` | `blockDim.x` |
| active mask | CUDA 的 active mask（`__activemask()`） |
| shared memory | `__shared__` |
| `bar.sync` | `__syncthreads()` |

附录 A.4 的 GPU 解释器就是按这张表实现的：一个 CUDA thread 解释
一条 GM32 lane，`__syncthreads()` 对应 `bar.sync`。

### 不是 cycle-accurate

本章的模型只定义**功能语义**：哪些 lane 执行、读写哪块内存、结果
是什么。它不定义：

- 一条指令几个周期；
- warp 调度器的优先级；
- shared memory bank 冲突的时序；
- global memory 的延迟和合并。

第 18-20 章会给出指令/访存统计和 occupancy 计算，但仍然不会声称
"和真实 T4 一样快"。性能数字只用于相对比较。

## 实现

### `simt-model.json`

```json
{
  "warp_size": 8,
  "warps_per_block": 4,
  "threads_per_block": 32,
  "sys_funct": { "exit": 0, "ret": 1 },
  "special_registers": [
    { "name": "laneid", "funct": 2 },
    { "name": "warpid", "funct": 3 },
    { "name": "blockid", "funct": 4 },
    { "name": "tid", "funct": 5 },
    { "name": "ntid", "funct": 6 },
    { "name": "nwarps", "funct": 7 },
    { "name": "activemask", "funct": 8 }
  ],
  "active_mask": { "bits": 8, "all_active": 255 },
  "address_spaces": [
    { "id": 1, "name": "global", "scope": "grid" },
    { "id": 2, "name": "shared", "scope": "block" },
    { "id": 3, "name": "private", "scope": "lane" },
    { "id": 4, "name": "constant", "scope": "grid", "readonly": true }
  ]
}
```

### `gpumin_model.py`

```python
from gpumin_model import Model
m = Model()

m.laneid(13)                 # 5
m.warpid(13)                 # 1
m.tid(1, 5)                  # 13
m.gid(2, 5)                  # 69
m.activemask([0, 2, 4, 6])   # 0x55
m.special("ntid", tid=0, blockid=0, active_lanes=[])  # 32
```

`--selftest` 检查：

- `warp_size * warps_per_block == threads_per_block`；
- `all_active == (1 << warp_size) - 1`；
- 对 0..31 的每个 `tid`，`tid(warpid(tid), laneid(tid)) == tid`；
- `gid(2, 5) == 69`；
- 交错的 mask（`0x55` / `0xaa`）；
- 七个特殊寄存器的名字和 `funct` 齐全。

`--check-doc` 反向检查本文档是否仍然写着模型里的数字。

### 与第 09 章 lowering 的关系

第 09 章已经把七个 `llvm.gpumin.*` intrinsic 选成 `sr.*`。本章不重复
实现，只确认：

```llvm
%v = call i32 @llvm.gpumin.laneid()
```

在 `llc` 里仍然是 `sr.laneid r4`，而它在执行时的值由模型给出：

```text
laneid = tid % 8
warpid = tid / 8
```

第 12 章的模拟器会把 `tid` 写进每条 lane 的上下文，然后按这张表
填充 `sr.*` 的返回值。

## 验证

`check-ch11.sh` 分四组：

1. **模型自检**：`gpumin_model.py --selftest`；
2. **编码一致性**：从 JSON 读取每个特殊寄存器的 `funct`，与
   `llvm-mc -show-encoding` 的字节比较；
3. **lowering 一致性**：七个 intrinsic 仍然各自选成 `sr.*`；
4. **文档一致性**：`--check-doc` 断言文档里的 `warp_size = 8`、
   `warps_per_block = 4`、`threads_per_block = 32`、
   `tid = warpid * 8 + laneid`、`gid = blockid * 32 + tid`、`0xff`、
   `active mask`、`每个 warp 一个 PC`、`uniform` 和四个地址空间名字
   都还在。

```sh
llvm/lib/Target/GPUMin/scripts/check-ch11.sh <llvm-bin-dir>
```

## 常见坑

1. **把 `laneid` 当成 0..31**。教学 warp 是 8 条 lane；0..31 是
   block 内的 `tid`。两者差一个 `warpid`。
2. **把 `tid` 当成全局线程号**。全局号是
   `gid = blockid * 32 + tid`；`tid` 只在 block 内有意义。
3. **active mask 用 32 位全 1**。全活跃是 `0xff`，不是
   `0xffffffff`；高 24 位没有对应 lane。
4. **把 active mask 写进谓词寄存器**。`p0` 只保存一条 lane 的 1 bit；
   mask 是 warp 共享的位图。
5. **以为"每个 lane 一个 PC"**。PC 是 warp 的；分歧时用 mask +
   SIMT stack 让不同 lane 子集走不同路径，而不是给每条 lane 一个
   独立 PC。
6. **把谓词当成分歧**。谓词只抑制一条指令；分歧是控制流走了不同
   分支。`@p` 不会改变 PC。
7. **把 per-lane 值写进 uniform 寄存器**。`ur` 每个 warp 一份，
   写入必须与 lane 无关；`laneid` 不能进 `ur`。
8. **barrier 不 uniform**。块内所有 lane 都必须到达 `bar.sync`；
   提前 `exit` 会让 CUDA 映射死锁。
9. **把 private 当成 block 共享**。private 是每条 lane 的栈；
   block 共享的是 `shared`（`addrspace(2)`）。
10. **用这个模型宣称性能**。它只定义功能语义；周期数、调度、
    合并访问都不在里面。

## 练习

1. **2D block**：把模型扩展成 `blockDim.x/y`，增加
   `tid.y`、`ntid.y`、`blockid.y`；想清楚哪些公式要乘 `ntid.x`。
2. **`gridid`/`nctaid`**：多 kernel 启动时，`blockid` 之外还需要
   网格维度；哪些值是 uniform？
3. **Python 小模拟器**：只实现 `MOVI`、`ADD`、`SETP`、`BRA_P`、
   `EXIT`，用 `gpumin_model.py` 初始化 8 条 lane 的 `laneid`，
   跑一个 `if/else` 并打印每步的 active mask。
4. **改 `warp_size`**：把 JSON 改成 4 或 16，跑 `check-ch11.sh`，
   看哪些测试/文档跟着红；这能暴露硬编码。
5. **CUDA 对照**：写一个 `blockDim.x = 32` 的 CUDA 程序，用
   `threadIdx.x % 8` / `/ 8` 打印 lane/warp，与模型输出逐项比较。
6. **mask 运算**：实现 `activemask & (1 << laneid)` 判断自己是否
   活跃；再实现 `__ballot_sync` 的雏形（第 22 章）。
7. **模型版本**：给 JSON 加 `version`，让第 12 章的模拟器在版本不
   匹配时拒绝启动，练习"数据/代码一起演进"。

## 下一章

第 12 章：**Python 模拟器**。它会解析 ELF32 的 `.text`，解码 32 位
GM32 指令，按本章的状态所有权建立每 lane 的 `r/p`、每 warp 的 `ur`
和 PC、每 block 的 shared memory，并执行 `vecadd`。第 10 章门禁 6
（模拟器 e2e）会在那一章从 `SKIP` 变成 `PASS`。
