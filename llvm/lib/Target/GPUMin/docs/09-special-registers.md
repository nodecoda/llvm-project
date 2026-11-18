# 第 09 章：特殊寄存器与 target intrinsic

## 目标

前八章的所有值都来自 IR：参数、load、常量、ALU 结果。SIMT 程序还需要
一类**不是普通值**的输入——这条 lane 是谁、它属于哪个 warp、哪个 block、
同 warp 里还有哪些 lane 活着。本章把这些状态接进 GM32：

1. 七个 SIMT 身份特殊寄存器：`laneid`、`warpid`、`blockid`、`tid`、`ntid`、
   `nwarps`、`activemask`；第 13 章在同一编码空间追加 `sr.args`
   （launch 参数块指针，来自 `ur0`）；
2. IR 入口是 target intrinsic：`llvm.gpumin.laneid()` 等，返回 `i32`；
3. 每个 selector 是 SYS opcode（`0x12`）的一个独立 `funct`，汇编写作
   `sr.<name> rd`，例如 `sr.laneid r1`；
4. `activemask` 是 **i32 位图**，bit i 表示 lane i 是否活跃，不是谓词
   寄存器；全活跃时教学配置下是 `0xff`；
5. 这些读都是纯操作，同一区域里的重复读会 CSE；但 `activemask` 故意
   不标 `IntrSpeculatable`，避免被提出分歧区域；
6. MC 编码、反汇编、CodeGen 选择、内核 `exit` 全部有测试。

退出条件（可执行）：

```sh
llvm/lib/Target/GPUMin/scripts/check-ch09.sh <llvm-bin-dir>
```

## 背景

### SIMT 状态不是普通寄存器

`laneid` 这类值的三个特点，决定了它不该进通用寄存器堆：

| 特点 | 后果 |
| --- | --- |
| 只读 | 不需要分配、不需要 spill，也不该出现在 SSA 的 def 链里 |
| 每 lane 不同 | 不是 uniform 值，不能放 `ur0-ur7` |
| 由启动配置决定 | 编译器不知道具体值，不能当常量折叠（除非做特化） |

如果硬把它做成 GPR，编译器必须在函数入口插入"读特殊寄存器"的初始化，
寄存器分配器会把它当普通活跃值搬运，函数调用还要决定它是否 callee-saved。
这些工作全是假的：硬件本来就有这条只读通路。

LLVM 的标准答案是 **target intrinsic**：IR 里是一个有类型、无函数体的
调用，后端把它模式匹配成一条机器指令。优化器能看见它的属性（无内存
副作用、是否可推测执行），但不会把它当普通函数调用去走 ABI。

### 三个恒等式

教学配置固定为 **4 warp × 8 lane = 32 thread/block**。七个值的关系是：

```text
tid    = warpid * 8 + laneid          // block 内线程号，0..31
ntid   = nwarps * 8                   // block 内线程数，32
nwarps = 4                            // block 内 warp 数
laneid = tid % 8                      // warp 内 lane 号，0..7
warpid = tid / 8                      // block 内 warp 号，0..3
```

`blockid` 来自网格启动（第 13 章）。全局线程号要到第 13 章才有意义：

```text
gid = blockid * ntid + tid
```

注意 `tid` 是 **block 内**的线程号，不是全局线程号；这是 CUDA
`threadIdx.x` 的语义，也是 PTX 的 `%tid.x`。

### 为什么 `activemask` 是 i32 位图

一个 warp 有 8 条 lane（教学模型）。active mask 需要同时表示 8 个布尔，
最自然的机器表示是一个 32 位整数：bit i 对应 lane i。全活跃时：

```text
lane:   7 6 5 4 3 2 1 0
bit:    1 1 1 1 1 1 1 1   = 0xff
```

它不是谓词寄存器：`p0-p7` 是**每条 lane 各有一份**的 1-bit 状态，而
active mask 是**整个 warp 共享**的位图。把 0xff 写进 `p0` 只会得到
"lane 0 的谓词 = 1"，其余 7 位丢失。第 14 章做分歧时，硬件/模拟器
会维护这个位图，并在分支处更新它。

### 为什么一个 selector 一条指令

第 07 章的 `addi` 说明过一个原则：编码里有空间放 `funct` 时，优先用
`funct` 区分语义，而不是让汇编器在运行期解析。这里同理。SYS opcode
的低 11 位已经用来区分 `exit`（0）、`ret`（1），继续往下排：

| funct | 指令 | 语义 |
| --- | --- | --- |
| 0 | `exit` | kernel 结束 |
| 1 | `ret` | device function 返回 |
| 2 | `sr.laneid` | warp 内 lane 号 |
| 3 | `sr.warpid` | block 内 warp 号 |
| 4 | `sr.blockid` | grid 内 block 号 |
| 5 | `sr.tid` | block 内线程号 |
| 6 | `sr.ntid` | block 内线程数 |
| 7 | `sr.nwarps` | block 内 warp 数 |
| 8 | `sr.activemask` | warp 活跃 lane 位图 |
| 9 | `sr.args` | launch 参数块指针（第 13 章，读 `ur0`） |
| 10 | `sr.shared` | block 共享窗口基址（第 16 章） |

备选方案是一条通用 `sr rd, n`，`n` 是 5 位 selector。它少八条指令定义，
但需要给汇编器写一个自定义操作数（解析 `laneid`/`warpid` 这些标识符），
打印器还要反向映射；而且 `n` 和 `funct` 的边界会变得模糊。本章选
"一 selector 一指令"：汇编、解码、打印、DAG 匹配全部由 TableGen 从同一份
`Inst{}` 描述生成，没有手写解析。

## 设计

### 编码

SYS 的寄存器读形式复用 `[31:26]=0x12`，把目标寄存器放在常规的
`[25:21]`，selector 放在 `funct[10:0]`：

```text
S: [31:26] 0x12 | [25:21] rd | [20:16] 0 | [15:11] 0 | [10:0] funct
```

实测编码：

```text
sr.laneid r1      -> 0x48200002  [0x02,0x00,0x20,0x48]
sr.warpid r1      -> 0x48200003  [0x03,0x00,0x20,0x48]
sr.blockid r1     -> 0x48200004  [0x04,0x00,0x20,0x48]
sr.tid r1         -> 0x48200005  [0x05,0x00,0x20,0x48]
sr.ntid r1        -> 0x48200006  [0x06,0x00,0x20,0x48]
sr.nwarps r1      -> 0x48200007  [0x07,0x00,0x20,0x48]
sr.activemask r1  -> 0x48200008  [0x08,0x00,0x20,0x48]
```

`rd` 是普通 5 位 GPR 字段，所以 `sr.laneid r7` 是
`0x48e00002`，`sr.activemask r31` 是 `0x4be00008`。

### 指令类

`GPUMinInstrFormats.td` 新增一个 SYS 单寄存器类：

```tablegen
class GPUMinSysR1<bits<6> op, bits<11> funct, dag outs, dag ins,
                  string asmstr, list<dag> pattern = []>
    : GPUMinInst<outs, ins, asmstr, pattern> {
  bits<5> rd;
  let Inst{31-26} = op;
  let Inst{25-21} = rd;
  let Inst{20-16} = 0;
  let Inst{15-11} = 0;
  let Inst{10-0} = funct;
}
```

它只比 `GPUMinR1` 少一个源寄存器：`sr` 的输入是"哪条特殊寄存器"，
而这个选择已经编码进 `funct`，不是运行时寄存器操作数。

### 指令定义与模式

`GPUMinInstrInfo.td` 用一个 multiclass 生成七个指令：

```tablegen
multiclass GPUMinSR<bits<11> funct, string mnemonic> {
  def "" : GPUMinSysR1<OPC_SYS.Value, funct, (outs GPR:$rd), (ins),
                       mnemonic#" $rd", []>;
}

defm SR_LANEID     : GPUMinSR<0x002, "sr.laneid">;
defm SR_WARPID     : GPUMinSR<0x003, "sr.warpid">;
defm SR_BLOCKID    : GPUMinSR<0x004, "sr.blockid">;
defm SR_TID        : GPUMinSR<0x005, "sr.tid">;
defm SR_NTID       : GPUMinSR<0x006, "sr.ntid">;
defm SR_NWARPS     : GPUMinSR<0x007, "sr.nwarps">;
defm SR_ACTIVEMASK : GPUMinSR<0x008, "sr.activemask">;
```

IR 侧一一对应：

```tablegen
def : Pat<(int_gpumin_laneid), (SR_LANEID)>;
def : Pat<(int_gpumin_warpid), (SR_WARPID)>;
def : Pat<(int_gpumin_blockid), (SR_BLOCKID)>;
def : Pat<(int_gpumin_tid), (SR_TID)>;
def : Pat<(int_gpumin_ntid), (SR_NTID)>;
def : Pat<(int_gpumin_nwarps), (SR_NWARPS)>;
def : Pat<(int_gpumin_activemask), (SR_ACTIVEMASK)>;
```

`int_gpumin_laneid` 是 TableGen 对 intrinsic 的节点名：`llvm.gpumin.laneid`
去掉 `llvm.`，把点换成下划线。因为读没有操作数，模式就是一个裸节点到
裸指令的匹配，不需要任何 C++ lowering。

### intrinsic 定义与属性

`llvm/include/llvm/IR/IntrinsicsGPUMin.td`：

```tablegen
let TargetPrefix = "gpumin" in {
  def int_gpumin_laneid : Intrinsic<[llvm_i32_ty], [],
                                    [IntrNoMem, IntrSpeculatable,
                                     IntrWillReturn]>;
  // warpid / blockid / tid / ntid / nwarps 同上

  def int_gpumin_activemask : Intrinsic<[llvm_i32_ty], [],
                                        [IntrNoMem, IntrWillReturn]>;
}
```

三个属性各管一件事：

| 属性 | 含义 | 谁受益 |
| --- | --- | --- |
| `IntrNoMem` | 不读写内存 | GVN/CSE 可以合并重复读 |
| `IntrWillReturn` | 一定会返回 | 循环/调用分析不会把它当可能不返回 |
| `IntrSpeculatable` | 可以安全推测执行 | LICM 可以把它提出循环 |

前六个是"每条 lane / 每次启动恒定"的值，可以推测执行；`activemask`
**不是**：warp 一旦分歧，不同分支里的 mask 不同。把它标成
`IntrSpeculatable`，LICM 就可能把读提到 `if` 之前，观察到的永远是
分歧前的 mask。所以它只保留 `IntrNoMem`——同一区域内的 CSE 仍然成立，
但不能越过控制流边界。

### target intrinsic 的三处接线

新增一个 target intrinsic 不只是写 `.td`。LLVM 的 intrinsic ID 是全局
生成的，需要三处共享文件：

1. `llvm/include/llvm/IR/IntrinsicsGPUMin.td`：定义 intrinsic 记录；
2. `llvm/include/llvm/IR/Intrinsics.td`：`include` 新文件，让
   `llvm-tblgen -gen-intrinsic-enums` 看到它；
3. `llvm/include/llvm/IR/CMakeLists.txt`：生成
   `IntrinsicsGPUMin.h`（`-intrinsic-prefix=gpumin`）。

第 3 步生成的头文件目前没有 C++ 直接 `include`——模式匹配走的是
TableGen 记录，C++ 里不需要 `Intrinsic::gpumin_laneid`。但保留它有两个
作用：CLI 工具/未来 clang builtin 可以直接引用；以及让"每个 target 前缀
一份枚举头"的约定保持一致。

## 实现

### 从 IR 到汇编

```llvm
declare i32 @llvm.gpumin.laneid()
declare i32 @llvm.gpumin.warpid()
declare i32 @llvm.gpumin.blockid()
declare i32 @llvm.gpumin.tid()
declare i32 @llvm.gpumin.ntid()
declare i32 @llvm.gpumin.nwarps()
declare i32 @llvm.gpumin.activemask()

define i32 @ids() {
  %a = call i32 @llvm.gpumin.laneid()
  %b = call i32 @llvm.gpumin.warpid()
  %c = call i32 @llvm.gpumin.blockid()
  %d = call i32 @llvm.gpumin.tid()
  %e = call i32 @llvm.gpumin.ntid()
  %f = call i32 @llvm.gpumin.nwarps()
  %g = call i32 @llvm.gpumin.activemask()
  %t0 = add i32 %a, %b
  %t1 = add i32 %c, %d
  %t2 = add i32 %e, %f
  %t3 = add i32 %t0, %t1
  %t4 = add i32 %t2, %g
  %r = add i32 %t3, %t4
  ret i32 %r
}
```

`llc -march=gpumin -verify-machineinstrs` 的实际输出（调度器会重排）：

```asm
ids:
	sr.tid r1
	sr.blockid r2
	add r1, r2, r1
	sr.warpid r2
	sr.laneid r3
	add r2, r3, r2
	add r1, r2, r1
	sr.nwarps r2
	sr.ntid r3
	add r2, r3, r2
	sr.activemask r3
	add r2, r2, r3
	add r4, r1, r2
	ret
```

没有一条 `call`：七个 intrinsic 全部在指令选择阶段变成了 `sr.*`。

### 纯读的 CSE

```llvm
define i32 @twice() {
  %a = call i32 @llvm.gpumin.laneid()
  %b = call i32 @llvm.gpumin.laneid()
  %r = add i32 %a, %b
  ret i32 %r
}
```

`-O2` 下：

```asm
twice:
	sr.laneid r1
	add r4, r1, r1
	ret
```

第二次读被 CSE 掉了。这不是后端的特殊处理，而是
`IntrNoMem + IntrWillReturn` 让 GVN 知道两次读等价。`activemask`
在同一基本块里也会这样合并；跨分支则不会，因为它没有
`IntrSpeculatable`。

### 内核里的 `exit`

同一批 intrinsic 在 `"gpumin_kernel"` 函数里照常选择，只是返回伪指令
从 `ret` 变成 `exit`：

```asm
kernel_ids:
	sr.tid ...
	sr.blockid ...
	...
	sr.activemask ...
	st.global.b32 ...
	exit
```

这也说明特殊寄存器读与 kernel/device 的返回约定是正交的：前者只影响
指令选择，后者只影响 `LowerReturn`。

## 验证

`check-ch09.sh` 分四组：

1. **编码**：七个 selector 的字节、`rd` 变化、未知 selector 被拒绝、
   `.o` 反汇编往返；
2. **选择**：七个 intrinsic 各自选成 `sr.*`，输出里没有 `call`；
3. **纯度**：两次 `laneid` CSE 成一条，kernel 仍以 `exit` 结束；
4. **对象一致性**：`llc -filetype=obj` 与重汇编 `.s` 的 `.o` 逐字节一致。

另外有两个 lit 测试：

```text
llvm/test/MC/GPUMin/special.s
llvm/test/CodeGen/GPUMin/intrinsics.ll
```

运行：

```sh
llvm-lit -v llvm/test/MC/GPUMin/special.s \
            llvm/test/CodeGen/GPUMin/intrinsics.ll
llvm/lib/Target/GPUMin/scripts/check-ch09.sh <llvm-bin-dir>
```

## 常见坑

1. **只写 `IntrinsicsGPUMin.td` 不够**。不在 `Intrinsics.td` 里
   `include`，`llvm.gpumin.laneid` 就不是已知 intrinsic；IR 解析器会把它
   当普通未声明函数，TableGen 也会报 `int_gpumin_laneid` 不存在。
2. **忘记 CMake 行不会立刻报错**。当前没有 C++ 引用
   `Intrinsic::gpumin_*`，所以不生成 `IntrinsicsGPUMin.h` 也能编过；
   但一旦未来 clang builtin 或 C++ lowering 引用它就会缺头文件。
3. **`funct` 是共享空间**。0/1 已经是 `exit`/`ret`，2-8 是本章；
   第 17 章的 `bar.sync` 必须另选一个不冲突的值（建议 `0x10`）。
4. **`activemask` 不是谓词**。它是 i32 位图；写进 `p0` 只保留 bit 0。
5. **laneid 范围是 0..7，不是 0..31**。教学模型一个 warp 8 条 lane；
   32 是 block 内线程数，不是 warp 宽度。模拟器、CUDA 映射、文档必须
   用同一套数字。
6. **`tid` 是 block 内的**。全局线程号是
   `blockid * ntid + tid`，第 13 章才引入网格。
7. **不要把 `activemask` 标成 `IntrSpeculatable`**。在第 14 章之前看不
   出问题；一旦有分歧，LICM 会把读提到分支外，语义就错了。
8. **`sr` 没有 chain**。它不参与内存序，也不保证和 load/store 的相对
   顺序；需要顺序时用数据依赖或 barrier。
9. **`nwarps`/`ntid` 是启动配置**。当前是运行时值，不是编译期常量；
   不要在没有特化信息时把它们折叠成 4/32。
10. **kernel 与 device function 的返回仍然分开**。`sr.*` 不会改变
    `LowerReturn` 的 `exit`/`ret` 选择。

## 练习

1. **2D 网格**：增加 `sr.blockidx.y`、`sr.blockidy`、`sr.ntid.y`，把
   `blockid` 扩展成两个分量；练习 `funct` 表的扩张和汇编助记符命名。
2. **通用 `sr rd, n`**：实现一个 5 位 selector 的自定义操作数类，支持
   `sr r1, laneid` 和 `sr r1, 2` 两种写法；比较它与"一 selector 一指令"
   在解码器、打印器、错误信息上的差异。
3. **`gridid`/`nctaid`**：为多 kernel 启动增加网格维度；思考哪些值是
   uniform（可以放 `ur`），哪些是 per-lane。
4. **启动配置特化**：给 `GPUMinSubtarget` 加 `-gpumin-block-size=32`
   之类的选项，把 `ntid`/`nwarps` 折叠成立即数；观察 intrinsic 的
   `IntrSpeculatable` 如何允许这种替换。
5. **`vote.any`/`vote.all`**：用 `activemask` 实现 warp 级投票
   （第 22 章的可选扩展）；先想清楚"参与投票的 lane"和"活跃 lane"
   是不是同一个集合。
6. **`clock`/`globaltimer`**：增加两个非纯特殊寄存器（每次读都可能
   不同），练习不给 `IntrNoMem`/`IntrSpeculatable` 时的优化行为。
7. **CSE 边界**：写一个 `if` 两边都读 `activemask` 的程序，对比
   `-O0`/`-O2` 的输出；再把它改成 `laneid`，观察 LICM 的差异。

## 下一章

第 10 章：**静态验收**。到目前为止，00-09 章已经覆盖了寄存器、MC、
函数、ALU、控制流、global memory、调用、类型和特殊寄存器。下一章不
增加新指令，而是把八项静态门禁、计数锁定和 IR→ELF 的端到端检查固定
下来，作为进入 SIMT 模型（第 11 章）和模拟器（第 12 章）之前的基线。
