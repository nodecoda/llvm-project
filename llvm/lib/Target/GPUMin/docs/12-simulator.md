# 第 12 章：在模型机上执行 GM32——ELF 加载、微型链接器与 VM

## 目标

前面十一章证明了两件事：GM32 能编码，IR 能降到 GM32。但 `llc` 产出的
是**可重定位对象**，不是可执行文件；一条 `call` 在对象里甚至还是
"偏移 0 + 一条重定位"。本章写一个能真正跑起来的模型机：

1. 读取 ELF32 little-endian：section table、`.symtab`、`.rela.text`；
2. 实现一个**微型静态链接器**：把指向 `.text` 的重定位就地解析；
3. 按第 02 章的四种格式解码 32 位指令字；
4. 按第 11 章冻结的状态所有权执行：PC/mask 属于 warp，寄存器属于
   lane，shared 属于 block，global 属于 grid；
5. 提供临时 launch ABI（参数放 `r4-r11`）和内存填充/转储；
6. 用四个 e2e 程序验收：`vecadd`、循环、递归、七个特殊寄存器。

退出条件（可执行）：

```sh
llvm/lib/Target/GPUMin/scripts/check-ch12.sh <llvm-bin-dir>
```

## 背景

### `.o` 不是程序

`llc -filetype=obj` 的输出是一个 ELF32 可重定位对象。以第 07 章的
`sum_to` 调用为例：

```text
00000050:  call 0
           00000050:  R_GPUMIN_21_PCREL  sum_to
```

指令字里的 21 位偏移是 0，真正的目标在重定位表里。链接器的工作是

```text
value = S + A - P
```

其中 `S` 是符号地址，`A` 是 addend，`P` 是重定位所在位置的地址。GM32
的 `call` 字段按**字**存放偏移，所以还要再除以 4。没有链接器，模拟器
就只能跑没有调用的程序；有了这三十行"微型链接器"，第 07 章的递归和
第 09 章的特殊寄存器才能一起验收。

### 为什么是 Python

第 11 章已经把数字收进 `scripts/simt-model.json`。模拟器直接
`import gpumin_model`，不再复制一套 warp 宽度或特殊寄存器编号。
Python 的代价是慢（附录 A.4 的 GPU 解释器比它快约三个数量级），
收益是可读、可断点、可在任何机器上跑。**正确性参照物**必须是后者；
性能选项是另一个东西。

### KMVP 的边界

本章实现的是 **KMVP**（Kernel Minimum Viable Product）执行模型：

- 一个 block 的 4 个 warp **串行**跑完；barrier 要到第 17 章才有意义；
- 分支必须 **warp 内一致**；分歧是第 14 章的主题，本章的语义保留在
  `--strict-uniform` 开关里；
- 没有 cache、没有流水线、没有 cycle 计数；
- 参数仍然是临时 ABI：`r4-r11`，最多 8 个；第 13 章换成 launch
  descriptor。

把这些边界写死在门禁里，比"看起来能跑"更重要：遇到分歧时模拟器
必须报错，而不是悄悄给出一个和硬件不一致的结果。

## 设计

### 加载：只取执行需要的部分

`ELFImage` 只解析四类信息：

| 信息 | 用途 |
| --- | --- |
| section table | 找到 `.text`、`.symtab`、`.strtab`、`.rela.text` |
| `.text` 字节 | 指令流；长度必须是 4 的倍数 |
| 符号表 | 把重定位里的符号下标变成地址 |
| `SHT_RELA` | `R_GPUMIN_*` 重定位 |

`.eh_frame` 里也有 `R_GPUMIN_32_PCREL`，但它的目标 section 是
`.eh_frame` 自己，属于 unwind 元数据。加载器只处理**目标为 `.text`**
的重定位，其余原样忽略。

### 微型链接器支持的重定位

| 编号 | 名称 | 写入字段 | 公式 |
| --- | --- | --- | --- |
| 1 | `R_GPUMIN_32` | 整个 32 位字 | `S + A` |
| 2 | `R_GPUMIN_32_PCREL` | 整个 32 位字 | `S + A - P` |
| 3 | `R_GPUMIN_16_PCREL` | 低 16 位 | `(S + A - P) / 4` |
| 4 | `R_GPUMIN_21_PCREL` | 低 21 位 | `(S + A - P) / 4` |

三个细节：

1. `P` 是**被重定位那条指令的地址**，不是 `pc + 4`。第 05 章的
   fixup 就是这么定义的：`Value = target - instruction address`。
2. 16/21 位字段存的是**字数**；字节地址必须先检查 4 字节对齐。
3. `call` 的返回地址是 `pc + 1`（下一条指令），而 `ret` 直接跳到
   `lr`。两者不要混。

### 状态所有权（沿用第 11 章）

| 状态 | 作用域 | 本章实现 |
| --- | --- | --- |
| PC | warp | `Warp.pc` |
| active mask | warp | `Warp.mask`，8 位 |
| `r0-r31` | lane | `Lane.r[32]`；`r0` 读恒 0 |
| `p0-p7` | lane | `Lane.p[8]` |
| `ur0-ur7` | warp | 预留，本章不执行 uniform 指令 |
| shared | block | `Simulator.shared`，每 block 清空 |
| private | lane | 每 lane 一块，`r30 = 栈顶` |
| global | grid | `Memory.global_mem` |
| constant | grid | 只读；写忽略 |

`r30` 在 lane 创建时初始化为 private 区大小，栈向低地址增长；`r31`
初始为 0，第一次 `call` 会覆盖它。递归能工作，靠的是每条 lane 自己
的 private 区和自己的 `lr` 副本。

### 执行循环

```text
while mask != 0 and steps < max_steps:
    word = text[pc]
    decode(word)
    execute on every active lane
```

分支是唯一的"整 warp 决策点"：

```text
take[i] = not has_pred or lane[i].p[pred]
if all(take):     pc += off
elif not any(take): pc += 1
else:             error("non-uniform branch")   # 仅 --strict-uniform
```

这条 `else` 是第 12 章最重要的断言。它把"分歧"从"结果可能不对"
变成"立即失败"。第 14 章把 `else` 换成"拆成两帧、各自继续跑"，
并把这一段原样搬进 `--strict-uniform`：本章的门禁和文档说的仍然是
这个模型，只是现在要用开关显式要回来。

### 指令子集

| 格式 | 指令 | 本章行为 |
| --- | --- | --- |
| R | `add/sub/and/or/xor/shl/shr/sar` | 32 位补码/位运算 |
| I | `movi`、`addi` | 16 位立即数符号扩展 |
| R | `mov`、`selp` | 寄存器/谓词选择 |
| R | `setp.*` | 有符号/无符号 6 种关系 |
| M | `ld/st.{global,shared,local,constant}.{b8,b16,b32}` | 字节寻址、窄加载扩展 |
| B | `bra`、`bra pN` | 16 位字偏移 |
| J | `call` | 21 位字偏移，写 `r31 = pc + 1` |
| SYS | `exit`、`ret`、`sr.*` | 见下表 |

`SYS` 的 `funct` 直接来自第 11 章的模型：

```text
0 exit      1 ret       2 laneid    3 warpid
4 blockid   5 tid       6 ntid      7 nwarps
8 activemask
```

`exit` 清掉当前 warp 的 active lane；`activemask` 读到的是**执行这条
指令时**的 mask，所以全活跃时是 `0xff`，高 24 位为 0。

### 内存

地址一律是**字节地址**，little-endian：

```text
ld.global.b32 rd, 8(r1)   -> 读 r1 + 8 起的 4 个字节
ld.global.s8  rd, 0(r1)   -> 读 1 个字节，符号扩展到 32 位
st.shared.b16 4(r1), r2   -> 写 2 个字节
```

四种空间用同一套越界检查；越界立即报错，不静默回绕。constant 空间
只读，写入被忽略（而不是 fault），这是本章的简化。

### launch 与参数

本章仍是临时 ABI：

```text
--arg 0x0 --arg 0x2000 --arg 0x4000 --arg 32
```

依次写入每条 lane 的 `r4, r5, r6, r7, ...`，最多 8 个。`--blocks N`
让每个 block 的 `blockid` 不同；`--threads` 必须是 8 的倍数且不超过
32，因为教学配置固定 4 warp × 8 lane。

第 13 章会把参数搬进 launch descriptor：kernel 从内存里读 grid/block
维度和参数指针，模拟器用一个 `--launch` 描述文件替代一串 `--arg`。
那时 `r4-r11` 只作为普通寄存器使用。

### e2e 程序

| 程序 | 形状 | 预期 |
| --- | --- | --- |
| `vecadd.ll` | `out[tid] = a[tid] + b[tid]` | `0, 4, 8, ...` |
| `loop.ll` | `sum_to(8 + warpid)` | 每个 warp 一个常数 |
| `fib.ll` | `fib(6 + warpid)` | `8, 13, 21, 34` |
| `special.ll` | 7 个特殊寄存器写 8 字记录 | `[lane, warp, block, tid, 32, 4, 255, 0]` |

`loop` 和 `fib` 的 trip count/递归深度在 **warp 内一致**，这是刻意
设计：第 12 章还没有分歧栈。用 `sum_to(tid)` 会在第一条
`setp/bra` 就触发 `non-uniform branch`——这正是第 14 章的入口。
（第 14 章起默认会执行这种程序，门禁里用 `--strict-uniform` 复现
本章的报错。）

`special.ll` 用 `gid = blockid * 32 + tid` 计算记录地址。32 是本章
固定 threads/block；如果改 `--threads`，这个程序需要同步改。

## 实现

### 文件结构

```text
tools/gpumin_sim.py
  ELFImage       sections, symbols, relocations, word(pc)
  Lane           r[32], p[8], private memory, special ids
  Warp           pc, mask, ur[8], active_lanes()
  Memory         load/store(space, size, signed)
  Simulator      blocks/threads/args, run(), dump(), fill()
  main()         CLI
```

`--dump-text` 把加载并**重定位后**的 `.text` 逐字打印成十六进制。
门禁用它和 `llvm-objdump -d` 的字节逐条对比，确保"模拟器看到的指令"
和"工具链看到的指令"是同一份。

### 解码

R 格式：

```text
[31:26] opcode | [25:21] rd | [20:16] rs1 | [15:11] rs2 | [10:0] funct
```

I 格式把 `[15:0]` 当 `simm16`；M 格式在 `[15:0]` 里再切
`imm11 | space | size | sign`；B 格式用 `[25] hasPred`、
`[24:22] pred`、`[15:0] off16`；J 格式用低 21 位。

反汇编由 LLVM 的生成式 decoder 负责，Python 侧只执行，不打印
助记符。两边的一致性由 `--dump-text` + `llvm-objdump` 保证。这样
Python 不需要再维护一张"指令名到语义"的表。

### 运行时抓到的一个编译器 bug

第 12 章第一次让 `fib.ll` 跑起来时，输出是 `6, 7, 8, 9`，不是
斐波那契数。反汇编看起来"很合理"：

```asm
movi r1, 1
setp.gt p0, r4, r1
bra p0, .LBB0_1
...
.LBB0_1:          # base
```

但 `p0 = (n > 1)`，branch 却跳到了 base；等于 `n > 1` 时返回 `n`。
根因不在 ISel，而在 `analyzeBranch()`：一个 block 以
`BRA_P pred, T` + `BRA F` 结尾时，它把 TBB/FBB 解释反了——把无条件
`BRA` 的目标当成了"条件为真"的目标。block placement 据此重排，谓词
不变、目标互换，分支就被静默取反。

修复只有几行：TBB 必须是 `BRA_P` 的目标，FBB 必须是随后 `BRA` 的
目标。修完之后 `loop`/`fib` 都正确，而且 `choose` 这种 if/else 还
少了一条多余的无条件跳转。这个例子说明：**lit 能锁住编码和形状，
锁不住运行时语义；模拟器是第二条独立的证据链。**

## 验证

四个 e2e 程序：

```sh
BIN=/home/ubuntu/llvm/builds/pr-ctpop/bin
cd llvm/lib/Target/GPUMin

$BIN/llc -march=gpumin -O1 -filetype=obj scripts/e2e/vecadd.ll -o /tmp/vecadd.o
python3 tools/gpumin_sim.py /tmp/vecadd.o --kernel vecadd \
  --fill 0x0=0,1,32 --fill 0x2000=0,3,32 \
  --arg 0x0 --arg 0x2000 --arg 0x4000 --arg 32 --dump 0x4000=32
# 0 4 8 12 ... 124

$BIN/llc -march=gpumin -O1 -filetype=obj scripts/e2e/loop.ll -o /tmp/loop.o
python3 tools/gpumin_sim.py /tmp/loop.o --kernel sum_kernel \
  --arg 0x4000 --dump 0x4000=32
# 28 x8, 36 x8, 45 x8, 55 x8

$BIN/llc -march=gpumin -O1 -filetype=obj scripts/e2e/fib.ll -o /tmp/fib.o
python3 tools/gpumin_sim.py /tmp/fib.o --kernel fib_kernel \
  --arg 0x4000 --dump 0x4000=32
# 8 x8, 13 x8, 21 x8, 34 x8
```

`special` 用两个 block，检查 64 条 8 字记录：

```sh
$BIN/llc -march=gpumin -O1 -filetype=obj scripts/e2e/special.ll -o /tmp/special.o
python3 tools/gpumin_sim.py /tmp/special.o --kernel special_kernel \
  --blocks 2 --arg 0x4000 --dump 0x4000=512
```

门禁的四组：

```text
PASS 1/4: ELF loading, decoding and .text relocations agree
PASS 2/4: vecadd runs for 1 warp, 4 warps and 2 blocks
PASS 3/4: loops, recursion and all seven special registers are correct
PASS 4/4: bad opcodes, undefined calls and divergence fail loudly
```

第 1 组把 `.text` 的每个字和 `llvm-objdump` 对比，并单独验证
`call` 的 21 位字段确实被微型链接器改写；第 4 组要求三种错误路径
都能给出可读信息：未知 opcode、未定义符号、非一致分支。

总验收在这一章之后应为：

```text
Gates: 7 passed, 0 failed, 1 skipped
```

其中 gate 7（调度/occupancy）要等第 18 章；gate 8 已经会检查本章的
文档、门禁和计数。

## 常见坑

1. **把 `P` 当成 `pc + 4`**。GM32 的 fixup 以指令地址为基准；写成
   `pc + 4` 会让每条 `call` 的目标整体后移一条指令。
2. **忘记除以 4**。16/21 位字段存的是字数；字节数直接写进去，跳转
   会落到指令中间。
3. **`analyzeBranch` 的 TBB/FBB 语义**。`BRA_P` 的真目标是 TBB，
   随后 `BRA` 的目标是 FBB；反过来会在 block placement 之后静默
   取反分支。
4. **认为 `call` 的下一条指令是 `pc + 4 字节`**。执行器里 `pc` 是
   字下标，返回地址是 `pc + 1`。
5. **在分歧 warp 上执行**。本章的模型会直接报错；不要为了"跑通"
   把检查删掉——第 14 章会用帧集合替换它，并把这个检查留在
   `--strict-uniform` 后面。
6. **`r0` 写入**。`write_r(0, ...)` 必须无效；否则零寄存器会被
   程序改掉，地址算术全乱。
7. **`--threads` 不是 8 的倍数**。教学机固定 8 lane/warp，模拟器
   在入口就拒绝，而不是产生半个 warp。
8. **`.eh_frame` 重定位**。它是 unwind 元数据，不要因为看到
   `R_GPUMIN_32_PCREL` 就拒绝整个对象；只看目标 section 是不是
   `.text`。
9. **constant 写入**。本章选择忽略而不是 fault；如果你把它改成
   fault，要同步改文档，否则第 16 章的测试会先撞上。
10. **把模拟器当性能模型**。它不建模 cycle、cache、warp 调度；
    `fib` 的步数只用于防止死循环。

## 练习

1. 用 `--trace` 跑 `fib`，手工核对一次 `call/ret`：`lr` 在什么时候
   被覆盖、递归返回后 `r4` 里是谁的结果。
2. 给 `ELFImage` 加 `R_GPUMIN_32` 的绝对地址用例，例如在 `.text`
   里放一个函数指针表；验证 `S + A` 与 `S + A - P` 的差别。
3. 给执行器加指令直方图（按 opcode 计数），比较 `vecadd`、`loop`、
   `fib` 的分布，解释为什么 `fib` 的 `call/ret` 占比高。
4. 用 `--blocks 8` 跑 `special`，把 `blockid` 字段画成表；再改
   `--threads 8`，说明为什么 `gid = blockid * 32 + tid` 需要同步改。
5. 写一个 9 个参数的 kernel，观察 `--arg` 的 8 个上限；然后列出
   第 13 章 launch descriptor 至少需要哪些字段。
6. 把 `non-uniform branch` 的报错位置（pc、mask、take 向量）补进
   错误信息，为第 14 章的 divergence stack 留调试接口。

## 下一章

第 13 章把临时 ABI 换成 **kernel launch descriptor**：grid/block 维度、
参数区指针、每个 kernel 的元数据都放进内存，模拟器用一个
`--launch` 文件描述一次启动。这样参数不再受 `r4-r11` 的 8 个限制，
也为第 16 章的 shared memory 和第 18 章的 occupancy 提供真实的
launch 形状。
