# 第 16 章：共享内存（地址空间 2）

## 目标

第 06 章只做了一件事：`addrspace(1)` 的普通全局内存，`ld.global.b32` /
`st.global.b32`，地址进符号表、进重定位。本章加第二种内存：

1. `addrspace(2)` 的**共享内存**：block 内所有 warp 可见，grid 不可见；
2. 给它一条新指令 `sr.shared`，kernel 用它拿到本 block 的窗口基址，
   而不是把地址写死；
3. 选择器按**指针的地址空间**挑 opcode，而不是只认 global；
4. 一个共享全局**不出现在对象文件里**：没有 `.bss`、没有符号、没有重定位；
5. 静态偏移按声明顺序 + 对齐分配，不引入额外的分配 pass。

退出条件（可执行）：

```sh
llvm/lib/Target/GPUMin/scripts/check-ch16.sh <llvm-bin-dir>
```

## 背景

### 为什么需要第三种内存

到第 15 章为止，GPUMin 只有全局内存。这不是能力问题，是**性能问题**：

- 全局内存的语义是"grid 可见"，所以它必须能被任意 block 寻址，
  于是它只能住在显存里，一次访问几百个周期；
- 一个 block 内的数据交换（归约、tile、转置）如果全走全局内存，
  等于把片上能做的事推到片外；
- 硬件提供的答案是**每个 block 一块私有的片上存储**：CUDA 叫 shared
  memory，OpenCL 叫 local，GPUMin 沿用 `addrspace(2)`。

它的三个性质决定了后面所有实现细节：

| 性质 | 后果 |
| --- | --- |
| block 作用域 | 地址在 block 内有效，跨 block 无意义 |
| 由 launch 运行时分配 | 编译器不能把它当静态存储放进对象文件 |
| 片上 | 延迟低，但容量固定（模型里 49152 字节/block） |

第二条是关键：**共享内存不属于对象镜像**。同一个 `.o` 可以在任意 grid
上启动，每个 block 都会拿到自己的窗口。所以编译器能给它的只有一个
"窗口内的偏移"，绝不能是一个链接期地址。

### 地址空间是编码的一部分

第 02 章定 M 格式时已经把地址空间写进了 opcode：

```text
[31:26] opcode | [25:21] rd/rs2 | [20:16] base | [15:5] imm11
[4:3] space | [2:1] size | [0] sign
```

`space` 两位，取值是 **地址空间编号减一**：

| IR 地址空间 | `space` 字段 | 助记符 |
| --- | --- | --- |
| `addrspace(1)` global | `0b00` | `ld.global.*` / `st.global.*` |
| `addrspace(2)` shared | `0b01` | `ld.shared.*` / `st.shared.*` |
| `addrspace(3)` private | `0b10` | `ld.local.*` / `st.local.*` |
| `addrspace(4)` constant | `0b11` | `ld.constant.*`（只读） |

把空间放进 opcode 而不是做成一个运行时字段，是第 02 章那个决定
（"每个 size/space 组合一个独立 opcode"）的直接结果：解码器不需要
任何位测试就能区分 `ld.shared.u16` 和 `ld.global.b16`。代价是 4 个
空间 × 5 种 load = 20 个 opcode，TableGen 用 `multiclass` 一行一个
空间生成，成本可忽略。

### 为什么 TableGen 模式不够

第 06 章的内存指令是模式选出来的：

```tablegen
def : Pat<(load GPR:$base), (LD_GLOBAL_B32 $base, 0)>;
```

这个模式看不见地址空间。`ISD::LOAD` 节点上带着 `MachineMemOperand`，
里面有地址空间信息，但 TableGen 的 `Pat` 只能匹配**操作数结构**，
拿不到节点属性。

有两个出路：

1. 拆出新的 SDNode（比如 `gpumin_shared_load`），在 lowering 里把
   shared 的 load 换掉，再写模式；
2. 在选择器里手写：`Select()` 里看 `LD->getAddressSpace()`，直接挑
   opcode。

本章选 2。理由：只有 4 个地址空间，映射是纯查表；方案 1 要多一个
DAG combine 阶段、多一套 TableGen 定义，还得处理"combine 之后又冒出来
一个新 load"的情况。教学上也更直白——**地址空间是一个属性，属性就该在
能看到属性的地方处理**。

## 设计

### 数据流

```text
@tile = internal addrspace(2) global [8 x i32]     IR
        |
        v
GlobalAddress(%tile) + 偏移                          SelectionDAG
        |
        v
sr.shared -> addi off                              机器指令
        |
        v
ld.shared.b32 rd, 0(base)                          机器指令
```

关键点：`GlobalAddress` 是**唯一**知道"这是个共享全局"的地方，所以
偏移分配也在它这里做。一旦它被降成 `sr.shared + addi`，后面的 load
只看到一个普通指针。

### 静态偏移分配

没有分配 pass，也没有链接器插一脚。规则只有两条：

```cpp
for (const GlobalVariable &GV : M.globals()) {
  if (GV.getAddressSpace() != AS_SHARED || GV.isDeclaration())
    continue;
  Next = alignTo(Next, GV.getAlign().valueOrOne());
  SharedOffsets[&GV] = Next;
  Next += DL.getTypeAllocSize(GV.getValueType());
}
```

1. 按**声明顺序**遍历（`Module::globals()` 保持文本顺序）；
2. 每个全局先按自身 `align` 向上取整，再占用 `sizeof` 字节。

于是布局对这个模块是确定的、可复现的，`llc` 跑两次偏移一样。它不是
最优的（真编译器会按大小排序以减少碎片），但它**可解释**，而这一章的
目的是让人看懂"偏移从哪来"。

一个具体例子（本章门禁第 4 组）：

```llvm
@a = internal addrspace(2) global [8 x i32] zeroinitializer, align 4   ; 0..32
@b = internal addrspace(2) global i8 zeroinitializer, align 1          ; 32..33
@c = internal addrspace(2) global [4 x i32] zeroinitializer, align 16  ; 48..64
```

`@c` 落在 48 而不是 33，因为 `alignTo(33, 16) == 48`。这三个数字被
门禁逐条断言，所以"对齐有没有生效"不是靠读代码相信，而是靠汇编验证。

### `sr.shared` 为什么必须存在

朴素的做法是让 `@tile` 直接等于常量 0。对固定的运行时（比如本仓库的
模拟器）它能工作，但它把"窗口基址"这个**运行时决策**焊进了编译产物。

真实的 launch 运行时至少有两个理由挪动这个基址：

- 一个 block 里可能有多个逻辑 tile，编译器看到的只是其中一个；
- 动态共享内存（`externdef` 那种）要在启动时按需求量分配。

所以编译器只承诺"我给你一个窗口内偏移"，基址由 `sr.shared` 在运行时
读出来。这是一条**契约**，不是优化：

```asm
sr.shared r1          # r1 = this block's shared window base
addi r1, r1, 28       # + static offset of the element
ld.shared.b32 r4, 0(r1)
```

模型里 `window_base = 0`（`scripts/simt-model.json` 的 `shared` 段），
模拟器、文档、门禁读同一个字段。换一个把窗口放在 0x1000 的运行时，
只需要改这一处。

### AsmPrinter 必须丢弃 AS2 全局

这是本章最容易漏掉的一步。`GlobalAddress` 降级之后，`@tile` 在
`llc` 眼里**没有任何引用了**，但它仍然是一个"有定义的全局变量"，
`AsmPrinter` 会照常给它发数据定义。第 16 章之前的输出长这样：

```asm
	.type	tile,@object
	.local	tile
	.comm	tile,32,4
```

对象文件里于是多了一个 32 字节的 `.bss` 符号。它没有重定位指向它，
所以程序还能跑，但它是**错的**：

- `.bss` 属于对象镜像，加载器会给它分配全局内存；
- 而这块存储应该由 launch 运行时按 block 分配；
- 一个 grid 有 N 个 block，就会有 N 份窗口，`.bss` 只有一份。

修法是在 `GPUMinAsmPrinter` 里加一个覆盖：

```cpp
void emitGlobalVariable(const GlobalVariable *GV) override {
  if (GV->getAddressSpace() == 2)
    return;
  AsmPrinter::emitGlobalVariable(GV);
}
```

门禁第 2 组断言三件事：`.s` 里没有 `.comm tile`，符号表里没有 `tile`，
重定位表里也没有。三条一起查，是因为"没有符号"和"没有重定位"是两件
事：符号可以留着但不被引用，重定位可以指向一个外部符号。

## 实现

### 1. 选择器：`GlobalAddress`

`Select()` 里新增的第一块：

```cpp
if (N->getOpcode() == ISD::GlobalAddress) {
  auto *GA = cast<GlobalAddressSDNode>(N);
  if (GA->getAddressSpace() == AS_SHARED) {
    int64_t Off = SharedOffsets[GA->getGlobal()] + GA->getOffset();
    SDValue Base = SDValue(
        CurDAG->getMachineNode(GPUMin::SR_SHARED, DL, MVT::i32), 0);
    SDNode *Addr = Base.getNode();
    if (Off != 0)
      Addr = CurDAG->getMachineNode(
          GPUMin::ADDI, DL, MVT::i32, Base,
          CurDAG->getTargetConstant(Off, DL, MVT::i32));
    ReplaceNode(N, Addr);
    return;
  }
}
```

`GA->getOffset()` 是常量 GEP 被折叠进来的部分，所以
`getelementptr ..., i32 0, i32 7` 直接得到 `Off = 0 + 28`，变成一条
`addi`，不需要额外的 add。

注意 `isInt<16>(Off)` 的检查：`addi` 的立即数是 16 位，共享窗口超过
32 KB 的模块会在这里显式报错，而不是悄悄截断。

### 2. 选择器：`LOAD` / `STORE`

```cpp
if (N->getOpcode() == ISD::LOAD) {
  auto *LD = cast<LoadSDNode>(N);
  unsigned AS = LD->getAddressSpace();
  if (AS != AS_GLOBAL) {
    unsigned Opc =
        pickLoadOpcode(AS, LD->getMemoryVT(), LD->getExtensionType());
    SDValue Ops[] = {LD->getBasePtr(),
                     CurDAG->getTargetConstant(0, DL, MVT::i32),
                     LD->getChain()};
    SDNode *Inst = CurDAG->getMachineNode(
        Opc, DL, CurDAG->getVTList(MVT::i32, MVT::Other), Ops);
    CurDAG->setNodeMemRefs(cast<MachineSDNode>(Inst), {LD->getMemOperand()});
    ReplaceNode(N, Inst);
    return;
  }
}
```

三个细节：

- `AS != AS_GLOBAL` 才接管：global 保持第 06 章的模式，行为一个字节
  都不变（门禁第 2 组专门断言这一点）；
- `pickLoadOpcode` 同时看**宽度**和**符号**：`i8` 且 `SEXTLOAD` 是
  `ld.shared.s8`，`ZEXTLOAD` 是 `ld.shared.u8`；
- `setNodeMemRefs` 不能省。少了它，后面的调度器和 `MachineInstr`
  就不知道这条指令访存了，`-verify-machineinstrs` 也不一定能抓到。

store 是对称的，只是多一个 `AS_CONST` 的拒绝：

```cpp
if (AS == AS_CONST)
  report_fatal_error("GPUMin: the constant space is read-only");
```

### 3. 组装：chain 必须放最后

本章唯一一个会让人调试半天的坑：

```cpp
// 对的
SDValue Ops[] = {Base, TargetConstant(0), Chain};
// 错的，会 assert
SDValue Ops[] = {Chain, Base, TargetConstant(0)};
```

原因是 `InstrEmitter::countOperands` 在把 DAG 操作数映射到 `MachineInstr`
时，**只在末尾**跳过 `MVT::Other`。chain 放中间，它就被当成一个普通
操作数，数量对不上，`getMachineNode` 之后的 `-verify-machineinstrs`
或者指令发射阶段直接炸。

规律可以记成一句：**`MVT::Other` 永远排在操作数列表最后**。glue 也是
一样的处理。

### 4. 模型与模拟器

`scripts/simt-model.json` 新增两个字段：

```json
"special_registers": [ ..., { "name": "shared", "funct": 10 } ],
"shared": { "window_base": 0, "bytes_per_block": 49152 }
```

模拟器里 `sr.shared` 的返回值直接来自模型：

```python
_special = { ..., "shared": self.model.shared_base }
```

访存另有一处细节值得说：模拟器的 `Memory.load` 用 `space == 0/1/2/3`
分支，这个数字就是指令里的两位 `space` 字段，也就是**地址空间编号减一**。
写反了不会崩，只会读到错的内存——所以门禁第 3 组直接比对输出值，而不是
只看有没有异常。

每个 block 有独立的 `bytearray(49152)`，所以两个 block 往同一偏移写
不会互相看见。这是第 13 章"block 私有状态"的自然延伸。

### 5. AsmPrinter

见上文"为什么必须丢弃"。实现只有四行，但它是"共享内存不属于对象镜像"
这条契约唯一能被机器检查的地方。

## 验证

四组门禁，每组针对一个具体断言：

| 组 | 断言 | 为什么值得单独一组 |
| --- | --- | --- |
| 1 | 模型、汇编器、选择器给出同一个 `sr.shared` | 一个指令描述在三处出现，必须对齐 |
| 2 | space 跟随指针；AS2 全局不进符号表/重定位 | "不占对象镜像"是本章的核心契约 |
| 3 | `shared_copy` 在模拟器上跑出 `17..10` | 值只在共享内存里存在过 |
| 4 | 偏移按声明顺序 + 对齐（32/48） | 分配规则必须可复现 |

第 1 组的三方对齐不是形式主义。`sr.shared` 的 funct 是 10，编出来是
`0x0a, 0x00, 0x20, 0x48`。这三个值分别来自：

- `simt-model.json` 的 `funct: 10`（文档与模拟器）；
- `GPUMinInstrInfo.td` 的 `GPUMinSR<0x00A, "sr.shared">`（汇编器）；
- `GPUMinISelDAGToDAG.cpp` 里对 AS2 的 `GlobalAddress` 处理（代码生成）。

任何一处改了而另外两处没改，这组门禁就红。

第 3 组的程序（`scripts/e2e/shared.ll`）值得读一遍：

```llvm
%pin = getelementptr i32, ptr addrspace(1) %in, i32 %t
%v = load i32, ptr addrspace(1) %pin
%pt = getelementptr [8 x i32], ptr addrspace(2) @tile, i32 0, i32 %t
store i32 %v, ptr addrspace(2) %pt          ; 存自己
%back = sub i32 7, %t
%pt2 = getelementptr [8 x i32], ptr addrspace(2) @tile, i32 0, i32 %back
%w = load i32, ptr addrspace(2) %pt2        ; 读 7-t
```

lane `t` 读回的是 lane `7-t` 写进去的值，所以输出必须是反序。
如果 `store` 和 `load` 被错误地映射到同一块内存（比如都走了 global），
或者静态偏移算错让 `pt2` 覆盖了别人的槽，输出就不是 `17..10`。

实测数字（1 block × 8 lane，输入 `10..17`）：

```text
17 16 15 14 13 12 11 10
issued=16 lane_instrs=128 loads=16 stores=16 branches=0 splits=0 frames_max=1
```

`loads` 是 16 而不是 8：每个 lane 一次 global load + 一次 shared load。
`splits=0`、`frames_max=1` 说明整个 kernel 是一条 lockstep warp——这正是
下一章之前的**安全边界**，见"常见坑"第 5 条。

## 常见坑

### 1. chain 排在操作数中间

见"实现 3"。症状是 `getMachineNode` 之后的选择器崩溃或者
`-verify-machineinstrs` 报操作数数量不符，原因却和内存无关，很容易往
`setNodeMemRefs` 上找。

### 2. 存储转发让门禁失去意义

第一版门禁第 2 组这么写：

```llvm
%v = load i32, ptr addrspace(1) %g
store i32 %v, ptr addrspace(2) %s
%w = load i32, ptr addrspace(2) %s     ; 期望看到 ld.shared.b32
```

`-O1` 把这段 forward 掉了：`store` 之后立刻读同一地址，值就在寄存器里，
共享 load 被删掉，于是"应该出现一条 `ld.shared.b32`"的断言永远失败。
更糟的情形是反过来——如果断言是"不能出现 ld.global"，转发会让它
**假通过**。

教训是通用的：**门禁要证明的是一个行为，不是一段文本**。改成每个函数
只做一次访存，断言才和指令选择真正相关。

### 3. 拓扑序让"折叠偏移"变成死代码

本章最初写了一个 `splitAddress()`：如果指针是 `(add base, const)`，就把
常量折进 M 格式的 11 位位移字段。写完发现它从不触发。

原因是 `SelectionDAGISel` 按**拓扑序**访问节点：走到 `LOAD` 时，它的
地址操作数已经被选择成机器节点了，`Ptr.getOpcode()` 不再是 `ISD::ADD`。

所以共享路径最终显式传 `imm = 0`：

```cpp
SDValue Ops[] = {LD->getBasePtr(), TargetConstant(0), Chain};
```

静态偏移已经在 `GlobalAddress` 那一步变成 `addi`，动态索引本来就是一个
寄存器 add，位移字段在这条路径上没有活干。11 位位移仍然被 global 的
模式（`simm11imm`）和 MC 测试覆盖，所以删掉死代码不是丢功能。

记一条：**在选择器里用 `ISD::*` opcode 去匹配操作数，基本都晚了一步。**

### 4. 只删引用，不删定义

见"AsmPrinter"一节。`llc` 不再引用 `@tile` 并不等于它会消失：只要
`AsmPrinter` 还在遍历 `Module::globals()`，它就是一个 `.bss` 定义。
症状很隐蔽——程序跑得对，只是对象文件里多了一块没人用的存储。

### 5. 没有 barrier 时只能指望 lockstep

`shared_copy` 之所以正确，不是因为"共享内存是同步的"，而是因为：

- 一个 warp 的 8 条 lane 在模拟器里**同一步执行同一条指令**；
- 所以 lane `t` 的 `store` 和 lane `t'` 的 `load` 之间存在确定的先后。

一旦跨 warp（4 个 warp 抢同一块窗口），或者 warp 内发生分歧导致
部分 lane 先跑到 `load`，这个顺序就没有保证了。此时唯一正确的做法是
插入一个 block 级 barrier——那是第 17 章的内容。

本章的程序因此刻意只用 1 个 warp（`--blocks 1 --threads 8`），并且
`check-ch16.sh` 断言 `splits=0`：**它在锁死自己的前提**。这不是偷懒，
是把"还没有同步原语"这件事写进门禁里。

## 练习

1. **动态共享内存**。加一个 `addrspace(2)` 的 `external` 全局，让运行
   时按 `--shared N` 分配，验证 `sr.shared` 不变、偏移从 0 开始。
2. **偏移排序**。把 `assignSharedOffsets` 改成按大小降序分配，观察
   `@b`（1 字节）在两种策略下的位置，并说明碎片差在哪。
3. **越界检查**。给 `ld.shared` / `st.shared` 加一条 `offset + size <=
   49152` 的编译期检查，写一个会触发它的测试。
4. **反向验证**。把 `emitGlobalVariable` 的覆盖删掉，跑门禁，确认第 2
   组会红——然后再加回来。门禁的价值在于它能红。
5. **参数化窗口基址**。把 `window_base` 改成 `0x1000`，跑一遍门禁，
   解释为什么所有断言仍然通过（提示：程序里没有一处依赖绝对地址）。

## 下一章

共享内存有了，同步还没有。第 17 章加 `bar.sync`：

- MC 层一条新指令；
- 模拟器从"warp 级串行"升级成"block 级交错"：4 个 warp 抢同一个
  `bytearray`，barrier 之前谁先谁后不确定；
- 于是 `shared_copy` 这类程序在没有 barrier 时会**随机**出错，而加上
  barrier 之后必须**每次都**正确——这会成为本章第 5 条坑的正式答案。

顺带一提，barrier 也是本仓库里 GPU 实验（`tools/gpu-sim-poc`）与
CUDA 语义贴得最近的一处：`bar.sync` 对应 `__syncthreads()`，而
"有些 lane 先 `exit` 再遇 barrier"在 CUDA 上会直接死锁。第 17 章的
模拟器要明确选择其中一种语义，并把选择写进模型文件。
