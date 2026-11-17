# 第 49 章：V 代码生成（二）——去重、常量折叠与归约

## 目标

第 48 章让 `v4i32` 真正走到了向量指令，但代价是**每条数据向量指令前都重发**
`vsetivli zero, 4, e32, m1, ta, ma`，而且常量只能先变成完整向量。本章在不改变
正确性边界的前提下，把这条链推进到“少冗余、可归约、可运行”：

1. **VL/VTYPE 去重**：同一基本块内，相同配置只发一次；块入口、调用之后、
   显式 `vset*` 之后重新配置；不做跨块数据流分析。
2. **常量与标量广播折叠**：5 位常量直接变成 `vadd.vi`/`vand.vi`/`vor.vi`/
   `vxor.vi`；超出范围或没有 `.vi` 形式的（`vsub`/`vmul`）用 `.vx`；
   变量广播用 `vmv.v.x`，并可被后续 ALU 折叠成 `.vx`。
3. **归约**：`llvm.vector.reduce.add.v4i32` 选择为
   `vmv.v.i 0` → `vredsum.vs` → `vmv.x.s`，返回标量。
4. **向量 ABI 边界不变**：向量参数/返回值继续明确拒绝。
5. **运行证据**：QEMU 8.2 跑通 `scripts/e2e-v/v-demo.c`；QEMU 6.2/7.2 无 V
   的记录保留为环境缺口。
6. 不改变第 45 章的 288 对象、clang 矩阵 432 对象；V 编码/反汇编计数从
   19 增加到 **27**。

范围仍是 **Zve32x 教学子集**：SEW=32、LMUL=1、VLEN=128、无 mask、无向量 FP。
不是完整 RVV。

**非目标**

- 跨基本块的 VL/VTYPE 数据流分析（第 50 章之后仍不做）。
- 非 splat 常量向量的常量池（继续走私有 16 字节栈槽 + `vle32.v`）。
- 向量比较/mask、LMUL≠1、strided/indexed、向量 FP、widening/narrowing。
- 向量 ABI（v8–v23 传参/返回）。
- 完整 RVV 的 `vsetvli` 优化器（上游 `RISCVInsertVSETVLI` 的等价物）。

## 背景：为什么“每条都发”可以变成“每块发一次”

`vsetivli` 写的是向量单元的状态：VL（本轮固定 4）和 VTYPE（e32/m1/ta/ma）。
任何一条数据向量指令都依赖这两项。第 48 章选择“每条指令前都发”，因为这是
最容易证明正确的做法：无论前一条指令是什么，配置都刚刚被重新写过。

第 49 章要省掉重复配置，就必须回答一个问题：**什么时候缓存一定失效？**

| 事件 | 为什么失效 |
| --- | --- |
| 基本块入口 | 前驱可能是任意分支目标，无法证明上一条配置 |
| `jal`/`jalr` 之后 | 被调函数可以改写 VL/VTYPE；返回地址也可能是任意路径 |
| 显式 `vsetivli`/`vsetvli`/`vsetvl` | 用户自己改了状态，缓存不再代表“本子集的配置” |
| 函数入口 | 调用者没有义务保留向量状态 |

反过来，**同一基本块内、两条数据向量指令之间**没有任何会改状态的指令，
所以缓存有效。这就是本章的去重边界：**块内缓存，块间保守**。

第二个动机是常量。`vadd.vv v8, v8, v9` 需要先把常量 5 变成 `v9`；但
`vadd.vi v8, v8, 5` 把常量放在指令里。DAG 选择是自底向上的：常量 splat
先被选成 `VMV_V_I`/`VMV_V_X`，等父节点 `ADD` 被选择时，操作数已经是机器
节点。此时如果认识这两个机器节点，就可以把“先造向量、再做 ALU”折叠成一条
`.vi`/`.vx`。

第三个动机是归约。默认 TTI 的 `shouldExpandReduction` 返回 true，
`ExpandReductions` 会在 IR 层把 `llvm.vector.reduce.add` 展开成
`extractelement` + 标量 `add`，SelectionDAG 根本看不到 `VECREDUCE_ADD`。
所以本章不仅要写 lowering，还要让 TTI 在 `+v` 下保留这一个 intrinsic。

## 代码地图

| 位置 | 本章职责 |
| --- | --- |
| `RVMinAsmPrinter.cpp:92` | `emitBasicBlockStart`：每个基本块入口让缓存失效 |
| `RVMinAsmPrinter.cpp:100` | `isVectorDataInstr`：按 `FmtV` 判断数据向量指令，排除 `vset*` |
| `RVMinAsmPrinter.cpp:120` | `emitVSETIVLI`：发射 `vsetivli zero, 4, 0xD0` |
| `RVMinAsmPrinter.cpp:128` | `emitMCInst`：缓存状态机；显式 vset / `JAL` / `JALR` 失效 |
| `RVMinAsmPrinter.cpp:249` | `VStateValid`：每个 MachineFunction 一个缓存位 |
| `RVMinISelDAGToDAG.cpp:396` | 在 `SelectCode(N)` 之前识别 splat 机器节点 |
| `RVMinISelDAGToDAG.cpp:403` | `getSplatImm`：必须是 `VMV_V_I` 机器节点 + 5 位常量 |
| `RVMinISelDAGToDAG.cpp:412` | `getSplatReg`：必须是 `VMV_V_X` 机器节点 |
| `RVMinISelDAGToDAG.cpp:435` | `selectSplat`：优先 `.vi`，否则 `.vx`（SUB/MUL 物化 GPR） |
| `RVMinISelLowering.cpp:440` | `lowerBUILD_VECTOR`：全同常量 → `VMV_V_I`；全同非恒定 → `VMV_V_X` |
| `RVMinISelLowering.cpp:539` | `lowerVECREDUCE_ADD`：`vmv.v.i` + `vredsum.vs` + `vmv.x.s` |
| `RVMinISelLowering.cpp:407` | `LowerOperation` 分派 `VECREDUCE_ADD` |
| `RVMinTargetTransformInfo.h:40` | `shouldExpandReduction`：`+v` 下保留 `vector_reduce_add` |
| `RVMinTargetMachine.cpp` | `getTargetTransformInfo()` 返回 RVMin 的 TTI |
| `RVMinInstrInfoV.td:280-293` | `.vx/.vi` 指令与 `VREDSUM_VS` |
| `RVMinInstrInfoV.td:337-355` | `vmv.v.x`（OPIVX，vs2=0，rs1=GPR） |
| `scripts/rt/link.ld:10-12` | `.rodata/.data/.bss` 页对齐，修 QEMU 8 的段权限 |
| `scripts/e2e-v/v-demo.c` | QEMU 8.2 的 `.vv/.vi/.vx/归约` 运行证据 |
| `test/CodeGen/RVMin/v-ext.ll:119-150` | `branch`/`aftercall` 的重新配置断言 |
| `test/MC/RVMin/v-extension.s` | 27 条编码 + 反汇编断言 |

## 关键实现

### 1. `vsetivli` 去重的状态机

AsmPrinter 里只加一个布尔量：

```cpp
bool VStateValid = false;

void emitBasicBlockStart(const MachineBasicBlock &MBB) override {
  VStateValid = false;
  AsmPrinter::emitBasicBlockStart(MBB);
}
```

`emitMCInst` 是所有发射路径的统一入口（伪指令展开、直接调用 lowering、
真实指令都会经过它），因此状态机放在这里：

```cpp
if (getSubtargetInfo().hasFeature(RVMin::FeatureStdExtV)) {
  unsigned Opc = Inst.getOpcode();
  if (Opc == RVMin::VSETIVLI || Opc == RVMin::VSETVLI ||
      Opc == RVMin::VSETVL) {
    VStateValid = false;          // 用户显式改了状态
  } else if (isVectorDataInstr(Opc)) {
    if (!VStateValid) {
      emitVSETIVLI();             // 只在缓存失效时发
      VStateValid = true;
    }
  }
  if (Opc == RVMin::JAL || Opc == RVMin::JALR)
    VStateValid = false;          // 调用/尾跳可能改写 VL/VTYPE
}
```

`isVectorDataInstr` 不能用“操作数里有 VR”判断：`vmv.x.s`/`vmv.s.x`
也有 VR，但它们是数据指令；`vset*` 没有 VR，却**不能**触发配置。
这里按 `TSFlags` 的低 5 位是否为 `FmtV`（17）判断，并显式排除三个 `vset`。

两个容易忽略的点：

- `JAL/JALR` 的失效发生在**发射之后**，所以 `aftercall` 里
  `vsetivli ... jalr ra, 0(t0) ... vsetivli ... vle32.v` 的顺序是对的。
- 压缩发生在 `emitMCInst` 的最后；原始 opcode 仍是 `JAL/JALR`，所以压缩成
  `c.jal`/`c.jr` 也不会漏掉失效。

### 2. `.vi`/`.vx` 折叠：先识别 splat 机器节点

选择器在 `SelectCode(N)` 之前拦截六种 ALU：

```cpp
if (N->getValueType(0) == MVT::v4i32 &&
    (N->getOpcode() == ISD::ADD || ... || N->getOpcode() == ISD::XOR)) {
  auto getSplatImm = [](SDValue V, int64_t &Imm) {
    if (!V->isMachineOpcode() || V->getMachineOpcode() != RVMin::VMV_V_I)
      return false;
    ...
    return isInt<5>(Imm);
  };
  ...
}
```

这里必须用 `isMachineOpcode()`/`getMachineOpcode()`。机器节点的
`getOpcode()` 返回的是 `ISD` 编号（本轮实测是 320），不是 TableGen 的
opcode；用 `V.getOpcode() == RVMin::VMV_V_I` 永远不成立。

折叠规则：

| 运算 | 有 `.vi` | 无 `.vi` |
| --- | --- | --- |
| add/and/or/xor | 5 位常量 → `.vi` | 其余 → `.vx` |
| sub | 无 | 5 位常量也要先 `materializeInt` 成 GPR，再 `.vx` |
| mul | 无 | 同上 |

对可交换运算（add/and/or/xor/mul），splat 在左在右都可以折叠；对 `sub`
只折叠 `vector - splat`，不把 `splat - vector` 错折成 `vsub.vx`。

`lowerBUILD_VECTOR` 同时处理两种“全同”：

```cpp
// 四个相同的 5 位常量 -> VMV_V_I
if (AllSame && isInt<5>(Value))
  return DAG.getMachineNode(RVMin::VMV_V_I, ...);

// 四个相同的非恒定值 -> VMV_V_X
if (AllIdentical)
  return DAG.getMachineNode(RVMin::VMV_V_X, DL, MVT::v4i32,
                            Op.getOperand(0));
```

### 3. `vmv.v.x`：变量广播

`vmv.v.x` 是 OPIVX（funct3=100），`vs2` 固定 0，`rs1` 放标量：

```tablegen
class RVMinVMVVXInst : RVMinInst<(outs VR:$vd), (ins GPR:$rs1),
                                 "vmv.v.x $vd, $rs1", []> {
  let Inst{14-12} = 0b100;   // OPIVX
  let Inst{24-20} = 0b00000; // vs2 = 0
  let Inst{25}    = 1;       // vm = 1
  let Inst{31-26} = 0b010111;
}
```

变量广播的 IR 常是 `insertelement` + `shufflevector`，DAG 会把它变成
`BUILD_VECTOR`/`SPLAT_VECTOR`；`lowerSPLAT_VECTOR` 和
`lowerBUILD_VECTOR` 都走 `VMV_V_X`，随后被第 2 节的折叠吃掉：

```asm
varbroadcast:
  vadd.vx v8, v8, a2
```

### 4. 归约：TTI 与 lowering 必须同时改

只写 `lowerVECREDUCE_ADD` 不够。默认 TTI 会让 IR 层把归约展开，DAG 里
根本不会出现 `VECREDUCE_ADD`。RVMin 的 TTI 只在这一个 intrinsic 上返回
false：

```cpp
bool shouldExpandReduction(const IntrinsicInst *II) const override {
  if (ST->hasStdExtV() &&
      II->getIntrinsicID() == Intrinsic::vector_reduce_add)
    return false;
  return true;
}
```

然后 `LowerOperation` 把 `VECREDUCE_ADD` 标成 `Custom`，lowering 发出三条
指令：

```cpp
SDValue Zero = DAG.getTargetConstant(0, DL, MVT::i32);
SDValue Seed = DAG.getMachineNode(RVMin::VMV_V_I, DL, MVT::v4i32, Zero);
SDValue Sum  = DAG.getMachineNode(RVMin::VREDSUM_VS, DL, MVT::v4i32,
                                  Vec, Seed);
return DAG.getMachineNode(RVMin::VMV_X_S, DL, MVT::i32, Sum);
```

`vredsum.vs vd, vs2, vs1` 的语义是“把 vs2 的元素求和，再加 vs1[0]”，
所以 vs1 必须先置 0；结果在 vd[0]，用 `vmv.x.s` 搬到 GPR。

TTI 需要 `Analysis` 与 `TransformUtils` 两个链接组件，否则
`RVMinTargetMachine.cpp` 里的 `getTargetTransformInfo()` 会在链接期报
undefined；`CMakeLists.txt` 已同步加上。

### 5. 向量 ABI 仍然拒绝

第 48 章的四个入口（`LowerFormalArguments`、`LowerCall`、`CanLowerReturn`、
`LowerReturn`）没有改：向量参数/返回值继续
`report_fatal_error("RVMin: vector arguments/returns are not supported yet")`。
第 49 章不引入向量调用约定，也不把向量静默拆成整数传递。

### 6. QEMU 8 的段权限：`.rodata` 不能和 `.text` 同页

QEMU 8.2 的 user-mode loader 按页设置权限。原来的 `link.ld` 把 `.rodata`
紧跟在 `.text` 后面，入口页同时含代码和只读数据，结果入口页失去执行权限，
程序在第一条指令就 SIGSEGV。修法不是加 `-z separate-code`，而是让数据段
各自页对齐：

```ld
.rodata : ALIGN(0x1000) { *(.rodata .rodata.*) *(.srodata .srodata.*) }
.data   : ALIGN(0x1000) { *(.data .data.*) *(.sdata .sdata.*) }
.bss    : ALIGN(0x1000) { *(.bss .bss.*) *(COMMON) }
```

这样 QEMU 6.2/7.2/8.2 都能跑；`.text` 的字节内容不变，第 45 章的
784→592 断言也不受影响。

## 构建与验证

```bash
B=/home/ubuntu/llvm/builds/pr-ctpop
W=/home/ubuntu/llvm/trees/pr-ctpop
ninja -C "$B" llc llvm-mc llvm-objdump clang
```

局部 lit：

```console
$ llvm-lit -s llvm/test/MC/RVMin llvm/test/CodeGen/RVMin
Total Discovered Tests: 35
  Passed: 35 (100.00%)
```

八项门禁（第 6 项仍是 216 次 llc，因为本章没有新增 `.ll` 文件）：

```console
PASS 2/8: lit (CodeGen + MC)
PASS 3/8: encodings
PASS 4/8: disassembler
PASS 5/8: e2e
PASS 6/8: 216 llc runs with -verify-machineinstrs
PASS 7/8: TableGen DSL contracts + status matrix + field effect
PASS 8/8: QEMU run (RV32I/M/A + F/D float/double + compressed demo)
ALL 8 GATES PASS
```

clang 矩阵：

```console
ALL CLANG MATRIX PASS: 432 objects, ELF32 EM_RISCV, -S/-c agree,
.text matches the riscv32 TargetInfo baseline
```

V 编码与反汇编：

```console
PASS: ... + 27 V instructions, .text identical
PASS: v (27 instructions, .text round-trips byte-for-byte)
```

运行证据（QEMU 8.2）：

```bash
B=/home/ubuntu/llvm/builds/pr-ctpop
R=$W/llvm/lib/Target/RVMin/scripts
T=$(mktemp -d)
"$B/bin/clang" --target=riscv32-unknown-elf -march=rv32imafdv -mabi=ilp32d \
  -O2 -ffreestanding -fno-builtin -S -emit-llvm "$R/e2e-v/v-demo.c" -o "$T/d.ll"
"$B/bin/llc" -march=rvmin -mtriple=riscv32-unknown-elf -O2 \
  -mattr=+f,+d,+v "$T/d.ll" -o "$T/d.s"
"$B/bin/llvm-mc" -triple=rvmin -mattr=+f,+d,+v -filetype=obj "$T/d.s" -o "$T/d.o"
for f in rvmin-rt.S start.S; do
  "$B/bin/llvm-mc" -triple=rvmin -mattr=+f,+d,+v -filetype=obj \
    "$R/rt/$f" -o "$T/${f%.S}.o"
done
ld.lld -m elf32lriscv -T "$R/rt/link.ld" -o "$T/d" \
  "$T/start.o" "$T/d.o" "$T/rvmin-rt.o"
/tmp/qemu8/usr/bin/qemu-riscv32-static "$T/d"
```

预期：

```text
rvmin: ok
exit=0
```

同一二进制在系统 QEMU（6.2）上：

```text
Illegal instruction (core dumped)
exit=132
```

这是环境缺口，不是代码生成错误；第 50 章会把“QEMU ≥ 8 运行”做成独立脚本，
默认门禁不依赖它。

## 预期输出

`add4`：一个基本块只配置一次。

```asm
add4:
  vsetivli zero, 4, e32, m1, ta, ma
  vle32.v v8, (a0)
  vle32.v v9, (a1)
  vadd.vv v8, v8, v9
  vse32.v v8, (a2)
```

`splat5`：常量被折叠进 `.vi`，没有独立 `vmv.v.i`。

```asm
  vadd.vi v8, v8, 5
```

`splat100`：超出 5 位范围，标量物化后走 `.vx`。

```asm
  addi a1, zero, 100
  vadd.vx v8, v8, a1
```

`varbroadcast`：变量广播不再造完整向量。

```asm
  vadd.vx v8, v8, a2
```

`reduce`：归约返回标量。

```asm
  vsetivli zero, 4, e32, m1, ta, ma
  vle32.v v8, (a0)
  vmv.v.i v9, 0
  vredsum.vs v8, v8, v9
  vmv.x.s a0, v8
```

`branch`：两个分支目标各自重新配置。

```asm
  vsetivli ...
  ...
  vsetivli ...
```

`aftercall`：`jalr` 之后重新配置。

```asm
  vsetivli ...
  jalr ra, 0(t0)
  vsetivli ...
  vle32.v ...
```

## 验收表

| ID | 标准 | 证据 | 结果 |
| --- | --- | --- | --- |
| V-CG12 | 同一基本块内相同 VL/VTYPE 只发一次 | `v-ext.ll` 的 `add4`：1 条 `vsetivli` | 通过 |
| V-CG13 | 分支目标处重新配置 | `v-ext.ll:119-137` 的 `branch`：2 条 | 通过 |
| V-CG14 | 调用后重新配置 | `v-ext.ll:139-150` 的 `aftercall`：`jalr` 后 1 条 | 通过 |
| V-CG15 | 5 位常量走 `.vi`，大常量走 `.vx` | `splat5` → `vadd.vi`；`splat100` → `addi`+`vadd.vx` | 通过 |
| V-CG16 | 变量广播走 `vmv.v.x` 并折叠成 `.vx` | `varbroadcast` → `vadd.vx ..., a2` | 通过 |
| V-CG17 | 归约返回标量 | `reduce`：`vmv.v.i 0`+`vredsum.vs`+`vmv.x.s` | 通过 |
| V-CG18 | 运行证据或明确缺口 | QEMU 8.2 `rvmin: ok`；6.2 SIGILL/132 | 通过（含缺口记录） |
| V-CG19 | 编码/反汇编 V 27 条；288/432/216 不变 | 门禁与矩阵输出 | 通过 |

## 常见坑

1. **机器节点不能用 `getOpcode()` 判断。** `SDValue::getOpcode()` 返回 ISD
   编号（本轮是 320），必须用 `isMachineOpcode()` +
   `getMachineOpcode()`；否则折叠永远不触发，看汇编只会看到 `vmv.v.i` +
   `vadd.vv`。
2. **TTI 不改，归约就“实现但不生效”。** `ExpandReductions` 在 IR 层展开，
   DAG 里没有 `VECREDUCE_ADD`；只写 lowering 会得到 4 个标量 `lw/add`。
3. **`vredsum.vs` 的种子必须是 0。** vs1[0] 会加到结果里；用未初始化的向量
   会得到随机值。`vmv.v.i 0` 既清 vs1，也顺便建立了 vset 配置。
4. **`sub` 不可交换。** 只折叠 `vector - splat`；`splat - vector` 需要
   `vrsub` 或换序，本章不做。
5. **`JAL/JALR` 失效要放在发射之后。** 放在之前会把“调用前的最后一条向量
   指令”也误判；放在之后才对应“返回后状态未知”。
6. **显式 `vset*` 之后必须重发本子集配置。** 用户可能写了不同的 VTYPE；
   本章不做“相同配置比较”，一律在下一数据指令前恢复成 e32/m1/ta/ma。
7. **`-verify-machineinstrs` 看不到 AsmPrinter 插入的 `vsetivli`。** 机器层
   验证通过不代表汇编里有配置；必须用 `V-NEXT`/`V-NOT` 文本断言。
8. **QEMU 8 的 SIGSEGV 不一定是代码错。** 先看 `link.ld` 的段对齐；入口页
   同时可写/只读/可执行时，QEMU 会按最后一页权限覆盖执行位。
9. **反汇编“坏”可能只是旧构建。** 本轮曾出现 `vsetivli` 被解成
   `vmv.x.s`，重链 `llvm-objdump`/`libLLVMRVMinDesc.so` 后消失；先做
   `llvm-mc` 单条最小复现，再怀疑 TableGen 解码表。
10. **不要改共享 e2e 语料。** 第 45 章的 288 对象和 clang 矩阵 432 是验收
    基线；V 的运行程序放在 `scripts/e2e-v/`，不加入 `scripts/e2e/*.c`。

## 练习

1. 把块内缓存升级成“基本块内的 VL/VTYPE 值编号”：只有配置相同才跳过，
   不同配置仍然重发（注意本章只有一种配置）。
2. 给 `splat100` 实现常量池：把 16 字节常量放 `.rodata`，用 `%hi/%lo`
   取地址后 `vle32.v`，比较指令数。
3. 实现 `vmv.v.x` + `vsub.vx` 之外的 `vrsub.vx`，支持 `splat - vector`。
4. 用 `vredsum.vs` 实现 `vredmax.vs`（需要新增 MC 指令与编码测试）。
5. 把 `aftercall` 扩展成“调用前后都有向量指令”，确认只有 `jalr` 后重发。
6. 用 `-regalloc=basic` 构造 33 个同时活跃的 `v4i32`，观察 16 字节 spill
   槽与 `vsetivli` 的相对位置。
