# 第 48 章：V 代码生成（一）——让 `v4i32` 真正走到向量指令

## 目标

第 47 章把 V 的 MC 层钉死了：19 条指令的编码、反汇编和诊断都可信，但
`llc -mattr=+v` 仍然生成标量代码。本章把这条链打通到代码生成：

1. `MVT::v4i32` 在 `+v` 下成为合法类型，一个值占一个 128 位向量寄存器；
2. 选择 `<4 x i32>` 的 `vle32.v`/`vse32.v`；
3. 选择 add/sub/mul/and/or/xor 的 `.vv` 形式；
4. 常量 splat：`-16..15` 用 `vmv.v.i`，其余走 16 字节栈槽 + `vle32.v`；
5. 支持向量寄存器 spill/reload（16 字节槽）；
6. 每条**数据向量指令**前保守发射
   `vsetivli zero, 4, e32, m1, ta, ma`；
7. 向量参数/返回值明确报错，不静默误编译；
8. 不改前端、不改变第 45 章的 288 对象与 clang 矩阵 432 对象计数。

范围仍是 **Zve32x 教学子集**：SEW=32、LMUL=1、VLEN=128、无 mask、无向量 FP。
不是完整 RVV。

**非目标**

- `<8 x i32>` 等其它宽度：仍由类型合法化拆分/标量化。
- 向量比较、mask、LMUL≠1、strided/indexed、向量 FP、widening/narrowing。
- VL/VTYPE 去重（第 49 章）。
- 向量 ABI（v8–v23 传参/返回）。
- QEMU/spike 运行：QEMU 6.2 不支持 V，本章只做静态与重汇编验证。

## 背景：为什么先做“保守但正确”的一条链

向量代码生成同时受三个系统约束：

| 系统 | 本章的选择 |
| --- | --- |
| 类型合法化 | 只让 `v4i32` 合法；其它向量操作显式 `Expand` |
| 指令选择 | load/store 手写地址物化；ALU 用 TableGen pattern |
| 机器层 | 16 字节 spill；`vsetivli` 在 AsmPrinter 插入 |

最容易犯的错误是“先优化”。例如先做 VL 去重、先做 `.vx/.vi`、先做向量 ABI，
任何一处都会把调试面扩大到三个系统同时出错。本章的顺序是：

```text
类型合法化 -> 选择 -> spill -> 保守 vsetivli -> ABI 拒绝
```

先让一条最简单的 `load/add/store` 生成正确汇编，再逐项加常量、extract 和
寄存器压力。第 49 章再在这个稳定基线上做 VL 去重、归约和常量池。

## 代码地图

| 位置 | 本章职责 |
| --- | --- |
| `RVMinISelLowering.cpp:51` | `addRegisterClass(MVT::v4i32, &VRRegClass)` |
| `RVMinISelLowering.cpp` 构造函数 | `+v` 下把未实现操作全部设为 `Expand` |
| `RVMinISelLowering.cpp:LowerOperation` | `BUILD_VECTOR`/`SPLAT_VECTOR`/`EXTRACT_VECTOR_ELT` |
| `RVMinISelLowering.cpp:lowerVectorThroughStack` | 私有 16 字节栈槽的 splat 路径 |
| `RVMinISelLowering.cpp:LowerFormalArguments/LowerCall` | 向量参数拒绝 |
| `RVMinISelLowering.cpp:CanLowerReturn/LowerReturn` | 向量返回值拒绝 |
| `RVMinISelDAGToDAG.cpp:matchBasePlusOffset` | 拆分 `base + C`（ADD/OR 两种形态） |
| `RVMinISelDAGToDAG.cpp:Select` | 向量 load/store 分支与地址物化 |
| `RVMinInstrInfoV.td` | 6 条 `.vv` pattern |
| `RVMinInstrInfo.cpp` | `copyPhysReg`、`storeRegToStackSlot`、`loadRegFromStackSlot` |
| `RVMinRegisterInfo.cpp:eliminateFrameIndex` | 识别“无立即数偏移”的 V 访存 |
| `RVMinAsmPrinter.cpp` | `isVectorDataInstr` + `emitVSETIVLI` |
| `test/CodeGen/RVMin/v-ext.ll` | 选择、splat、extract、spill、重汇编 |
| `test/CodeGen/RVMin/v-ext-abi-{arg,ret}.ll` | 向量 ABI 诊断 |
| `scripts/e2e-v/vector.c` | clang → IR → llc 的端到端冒烟 |

## 关键实现

### 1. 只让 `v4i32` 合法，其余显式 `Expand`

`addRegisterClass` 必须发生在 `computeRegisterProperties()` 之前，否则类型
动作已经算完，`v4i32` 仍会被当成非法类型拆成 4 个 i32。

```cpp
if (STI.hasStdExtV())
  addRegisterClass(MVT::v4i32, &RVMin::VRRegClass);
computeRegisterProperties(STI.getRegisterInfo());
```

之后从“全部 Expand”开始，只把本章真正实现的六个 ALU 操作改成 `Legal`：

```cpp
for (unsigned Op = 0; Op < ISD::BUILTIN_OP_END; ++Op)
  setOperationAction(Op, MVT::v4i32, Expand);
setOperationAction({ISD::ADD, ISD::SUB, ISD::MUL, ISD::AND, ISD::OR,
                    ISD::XOR},
                   MVT::v4i32, Legal);
setOperationAction(ISD::BUILD_VECTOR, MVT::v4i32, Custom);
setOperationAction(ISD::SPLAT_VECTOR, MVT::v4i32, Custom);
setOperationAction(ISD::EXTRACT_VECTOR_ELT, MVT::v4i32, Custom);
```

“先全部 Expand”比逐项枚举更安全：新增一个未实现操作时，默认是展开，而不是
带着非法节点进入选择器。代价是这一章没有向量移位、除法、比较、FP 和 mask。

### 2. 向量 load/store 没有立即数偏移

`VLE32_V`/`VSE32_V` 的操作数只有基址寄存器：

```text
vle32.v vd, (rs1)
vse32.v vs3, (rs1)
```

所以不能照搬标量 `LW rd, imm(rs1)` 的地址匹配。本章在 `Select()` 里先于标量
分支处理 `v4i32`：

| IR 地址 | DAG 形态 | 选择 |
| --- | --- | --- |
| `ptr %p` | GPR/机器节点 | 直接作 base |
| `gep ..., 4` | `ADD/OR Base, 4` | `ADDI Base, 4` |
| `alloca` | `FrameIndex` | `ADDI TFI, 0`，交给帧消除 |
| `alloca + 4` | `FrameIndex + 4` | `ADDI TFI, 4` |
| 变量索引 GEP | 已选成机器节点 | 直接作 base |

`matchBasePlusOffset` 同时接受 `ADD` 和 `OR`：DAGCombine 常把
`FI + C` 变成 `FI | C`，只匹配 `ADD` 会漏掉栈地址。偏移超过 12 位时用
`materializeInt` + `ADD`，与标量路径一致。

### 3. 算术/逻辑：只做 `.vv`

```tablegen
def : Pat<(v4i32 (add VR:$vs2, VR:$vs1)), (VADD_VV VR:$vs2, VR:$vs1)>;
def : Pat<(v4i32 (sub VR:$vs2, VR:$vs1)), (VSUB_VV VR:$vs2, VR:$vs1)>;
def : Pat<(v4i32 (mul VR:$vs2, VR:$vs1)), (VMUL_VV VR:$vs2, VR:$vs1)>;
def : Pat<(v4i32 (and VR:$vs2, VR:$vs1)), (VAND_VV VR:$vs2, VR:$vs1)>;
def : Pat<(v4i32 (or  VR:$vs2, VR:$vs1)), (VOR_VV  VR:$vs2, VR:$vs1)>;
def : Pat<(v4i32 (xor VR:$vs2, VR:$vs1)), (VXOR_VV VR:$vs2, VR:$vs1)>;
```

`.vx/.vi` 留给第 49 章的标量广播优化。现在常量会先变成完整向量（`vmv.v.i`
或栈槽 + `vle32.v`），再做 `.vv`；指令数不是最优，但语义和选择链是完整的。

### 4. 常量 splat 与 extract

**splat** 有两条路：

1. 四个元素是同一个常量且 `isInt<5>(C)`：`VMV_V_I`；
2. 其它情况：`CreateStackObject(16, Align(16))`，写 4 个 i32，再用
   `v4i32` load 读回，选择成 `VLE32_V`。

用私有栈槽而不是常量池，是为了避免本章引入 `.rodata`、PC 相对寻址和重定位。
常量池留给后续优化。

**extract**：

- 下标 0：`VMV_X_S`，直接向量→GPR；
- 下标 1..3：把整个向量 `VSE32_V` 到私有槽，再 `LW` 目标元素；
- 变量下标：`report_fatal_error`，本章不承诺。

### 5. spill/reload：16 字节、无立即数偏移

`VR` 是 128 位寄存器类，spill 槽自然是 16 字节、16 对齐。V 访存没有立即数
偏移，所以 `storeRegToStackSlot`/`loadRegFromStackSlot` **不能**加
`.addImm(0)`：

```cpp
MachineInstrBuilder MIB = BuildMI(MBB, MI, DL, get(Opc))
                              .addReg(SrcReg, getKillRegState(IsKill))
                              .addFrameIndex(FrameIndex);
if (!IsVector)
  MIB.addImm(0);
```

`eliminateFrameIndex` 也要识别这种“帧索引是最后一个操作数”的形态：

```cpp
bool HasImm = FIOperandNum + 1 < MI.getNumOperands() &&
              MI.getOperand(FIOperandNum + 1).isImm();
```

- `Offset == 0` 且没有立即数：直接把 FI 换成 `sp`；
- 否则走原有的 `materializeAddress` 虚拟 GPR 路径。

`copyPhysReg` 用 `VMV_V_V`，AsmPrinter 会在它前面补 `vsetivli`。

### 6. 保守 `vsetivli`：每条数据向量指令前都发一次

本章不做 VL/VTYPE 分析。AsmPrinter 在 `emitMCInst` 的统一入口判断：

```cpp
if (getSubtargetInfo().hasFeature(RVMin::FeatureStdExtV) &&
    isVectorDataInstr(Inst.getOpcode()))
  emitVSETIVLI();
```

`isVectorDataInstr` 排除 `vsetivli/vsetvli/vsetvl`，其余用 `FmtV`（TSFlags
低 5 位为 17）识别。配置序列固定为：

```text
vsetivli zero, 4, e32, m1, ta, ma
```

编码是 `57 70 02 cd`，操作数顺序 `(rd=X0, uimm=4, vtypei=0xD0)`。

这个做法有三个明确后果，文档和测试都要接受：

1. 指令数翻倍：第 49 章通过基本块内去重解决；
2. `-verify-machineinstrs` **看不到** `vsetivli`：它是 MC 层插入的，不是
   MachineInstr；测试断言汇编文本，不伪装成机器层验证；
3. inline asm 不经过 `emitMCInst`：手写向量指令需要自己写 `vsetvli`。

### 7. 向量 ABI：明确拒绝

向量参数/返回值需要 v8–v23 传参、栈对齐和 callee-saved 规则。本章在四个
入口检查：

- `LowerFormalArguments`：`Arg.VT.isVector()` → arguments；
- `LowerCall`：`Outs`/`Ins` 任一 vector → arguments/returns；
- `CanLowerReturn`：`RetTy->isVectorTy()` 或 `Outs` vector → returns；
- `LowerReturn`：防御性再查一次。

诊断走 `report_fatal_error`，保证 `llc` 非零退出：

```text
LLVM ERROR: RVMin: vector arguments are not supported yet
LLVM ERROR: RVMin: vector returns are not supported yet
```

静默按整数传递会误编译，因此“拒绝”本身是本章的验收项。

## 构建与验证

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop
ninja -C "$B" llc clang llvm-mc llvm-objdump llvm-readelf llvm-objcopy FileCheck
```

最小冒烟：

```llvm
define void @add4(ptr %out, ptr %a, ptr %b) {
  %va = load <4 x i32>, ptr %a, align 16
  %vb = load <4 x i32>, ptr %b, align 16
  %v = add <4 x i32> %va, %vb
  store <4 x i32> %v, ptr %out, align 16
  ret void
}
```

```console
$ llc -march=rvmin -mtriple=riscv32-unknown-elf -mattr=+v -O2 -verify-machineinstrs v.ll -o -
        vsetivli zero, 4, e32, m1, ta, ma
        vle32.v v8, (a2)
        vsetivli zero, 4, e32, m1, ta, ma
        vle32.v v9, (a1)
        vsetivli zero, 4, e32, m1, ta, ma
        vadd.vv v8, v9, v8
        vsetivli zero, 4, e32, m1, ta, ma
        vse32.v v8, (a0)
```

重汇编：

```bash
llc ... -mattr=+v v.ll -o - \
  | llvm-mc -triple=riscv32 -mattr=+v -filetype=obj -o /dev/null
```

局部 lit：

```console
$ llvm-lit -s llvm/test/MC/RVMin llvm/test/CodeGen/RVMin
Total Discovered Tests: 35
  Passed: 35 (100.00%)
```

clang 端到端（`scripts/e2e-v/vector.c`，不进共享 288 语料）：

```bash
clang --target=rvmin-unknown-elf -march=rv32imafdv -mabi=ilp32d -O2 \
      -S -emit-llvm llvm/lib/Target/RVMin/scripts/e2e-v/vector.c -o /tmp/vector.ll
llc -march=rvmin -mtriple=riscv32-unknown-elf -mattr=+v,+d -O2 /tmp/vector.ll -o -
```

> IR 带 `ilp32d` 的 module flag，所以 llc 需要 `+d` 满足 ABI 检查；
> 向量指令本身只来自 `+v`。

## 预期输出

编码/反汇编计数不变：

```text
PASS: 85 RV32I + 12 M + 19 Zbb + 8 Zbs + 19 A + 28 F + 28 D + 35 C + 24 Zicsr/Zifencei + 19 V instructions, .text identical
PASS: v (19 instructions, .text round-trips byte-for-byte)
PASS: 191 encoding-corpus instructions + 35 compressed + 6 mixed + 24 zicsr/zifencei + 6 label-resolved branches/jumps
```

e2e 与 clang 矩阵不变：

```text
PASS: 12 programs x 0 1 2 3 optimization levels x 6 feature sets
      = 288 objects, every .text identical to its llvm-mc reassembly
ALL CLANG MATRIX PASS: 432 objects
```

八项门禁（第 6 项从 204 增加到 216，因为新增 `v-ext.ll` 的 4 个优化级 ×
3 个无 V 特性集）：

```text
PASS 6/8: 216 llc runs with -verify-machineinstrs
ALL 8 GATES PASS
```

## 验收表

| ID | 标准 | 证据 | 结果 |
| --- | --- | --- | --- |
| V-CG1 | `+v` 下 `<4 x i32>` 的 load/store 生成 `vle32.v`/`vse32.v` | `v-ext.ll` 的 `add4` | 通过 |
| V-CG2 | add/sub/mul/and/or/xor 生成对应 `.vv` | `v-ext.ll` 的 `ops` | 通过 |
| V-CG3 | `-16..15` 的 splat 生成 `vmv.v.i` | `v-ext.ll` 的 `splat5` | 通过 |
| V-CG4 | 超出 5 位立即数的 splat 走栈槽 + `vle32.v` | `v-ext.ll` 的 `splat100` | 通过 |
| V-CG5 | 下标 0 的 extract 生成 `vmv.x.s`；1..3 走 `vse32.v`+`lw` | `v-ext.ll` 的 `extract0/1` | 通过 |
| V-CG6 | 32 个以上同时活跃的向量触发 16 字节 spill/reload | `v-ext.ll` 的 `spill` + `SPILL` 前缀 | 通过 |
| V-CG7 | 每条数据向量指令前都有 `vsetivli zero, 4, e32, m1, ta, ma` | `V-NEXT` 断言 | 通过 |
| V-CG8 | 输出可被 `rvmin +v` 和 `riscv32 +v` 重汇编 | 两条 `llvm-mc` RUN 行 | 通过 |
| V-CG9 | 无 `+v` 时同一 IR 仍能标量化编译 | 第 6 项门禁 216 次 llc 运行 | 通过 |
| V-CG10 | 向量参数/返回值明确报错 | `v-ext-abi-arg.ll`/`v-ext-abi-ret.ll` | 通过 |
| V-CG11 | 288 对象、432 clang 对象、191 条编码计数不变 | 门禁与矩阵输出 | 通过 |

## 常见坑

1. **`addRegisterClass` 的位置。** 必须在 `computeRegisterProperties()` 之前；
   放在后面类型动作不会更新，`v4i32` 仍会被拆分。
2. **私有栈槽的链不能用 `DAG.getRoot()`。** 当被 lower 的
   `BUILD_VECTOR` 正好供一个 store 使用时，root 已包含那个 store；从 root
   开始接新 store 会形成环，报 `Detected cycle in SelectionDAG`。私有槽不会
   逃逸，从 `DAG.getEntryNode()` 开始接链即可。
3. **不要给 V 访存硬加“不参与编码的 imm”。** 多一个 MCInst 操作数会让编码器
   和打印器都要特判；正确做法是把偏移变成 `ADDI`。
4. **`eliminateFrameIndex` 的 `HasImm` 必须检查操作数个数。** V 访存的帧索引
   后面没有立即数，直接读 `FIOperandNum + 1` 会越界或读到 chain。
5. **`storeRegToStackSlot` 不要给 V 路径加 `.addImm(0)`。** 这是标量
   `SW/LW` 的形状，V 指令没有这个字段。
6. **`DAG.getMachineNode` 建 V 指令时结果类型要写全。** load 是
   `{v4i32, Other}`，store 只有 `Other`；漏写 `Other` 会让链断开。
7. **`-verify-machineinstrs` 不验证 `vsetivli`。** 它在 AsmPrinter 插入；
   机器层验证通过不代表汇编里有配置序列，必须单独断言文本。
8. **QEMU 6.2 没有 V。** `vsetivli` 会直接 `Illegal instruction`；本章不要
   声称已运行，运行证据留给第 49/50 章（升级 QEMU ≥ 8 或 spike）。
9. **不要只测“常量向量 store”。** DAGCombine 可能把它拆成 4 个 `sw`，看起来
   像 `BUILD_VECTOR` 没实现；测试要让常量参与向量运算。

## 练习

1. 把 `splat100` 改成常量池实现：新增 `.rodata` 和 PC 相对地址，比较指令数。
2. 给 `splat` 增加 `.vx` 形式：标量广播用 `vmv.v.x`，避免先构造完整向量。
3. 实现下标 1..3 的 `extract` 不落栈：用 `vslidedown.vi` + `vmv.x.s`。
4. 给 `copyPhysReg` 加一个“同寄存器不发射”的断言，观察 RA 何时真的需要
   `vmv.v.v`。
5. 在 `emitVSETIVLI` 里缓存“当前 VL/VTYPE”，实现基本块内去重，为第 49 章
   做准备。
6. 写一个 `-regalloc=basic` 的 MIR 测试，直接构造 33 个同时活跃的
   `v4i32` 虚拟寄存器，观察 spill 槽偏移。
