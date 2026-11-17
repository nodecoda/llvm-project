# 第 41 章：D 扩展与 ILP32D 调用约定

## 目标

第 40 章把 `float` 从“i32 对 + libcall”变成了 FPR32 上的单条指令；`double`
仍然是软浮点。本章打开 **D 扩展**，完成之后：

1. `-mattr=+d` 下 `fadd.d/fsub.d/fmul.d/fdiv.d/fsqrt.d` 各一条指令；
2. `fabs/fneg/fcopysign` 选成 `fsgnjx.d/fsgnjn.d/fsgnj.d`，`fmin/fmax`
   选成 `fmin.d/fmax.d`，`fma` 选成 `fmadd.d`；
3. `fcvt.w.d/fcvt.wu.d`（RTZ）、`fcvt.d.w/fcvt.d.wu`、`fcvt.s.d/fcvt.d.s`
   以及 `feq.d/flt.d/fle.d/fclass.d` 全部可用；
4. f64 形参走 `fa0–fa7`、返回值走 `fa0/fa1`，`fs0–fs11` 在 D 下按 8 字节
   槽保存；
5. `clang --target=rvmin-unknown-elf -march=rv32id -mabi=ilp32d` 端到端可用；
6. 不开 D 时输出与第 40 章**逐字节一致**（F 的代码生成不受影响）；
7. 28 条 D 指令的编码与参考 `llvm-mc -triple=riscv32 -mattr=+f,+d`
   逐字节相同。

本章只做双精度标量浮点。第四篇的验收（门禁 8 项、clang 矩阵、QEMU 浮点运行）
在第 42 章。

## 背景：RV32 上的 double 为什么更麻烦

F 章的两个“顺手”条件，在 D 上都不成立：

| 问题 | F（第 40 章） | D（本章） |
| --- | --- | --- |
| 值宽度 | 32 位，正好一个 GPR | 64 位，RV32 没有 i64 寄存器类 |
| 与整数互搬 | `fmv.w.x`/`fmv.x.w` 单条指令 | **没有** `fmv.d.x`/`fmv.x.d`，只能过栈 |
| 常量 | `LUI+ADDI` 物化 i32，再 `fmv.w.x` | 8 字节栈槽 + 两条 `sw` + `fld` |
| select | bitcast 成 i32，复用整数 select | 两个 8 字节栈槽，选地址再 `fld` |
| ABI | `ilp32f` | `ilp32d`（D 隐含 F） |

还有一个寄存器文件层面的事实：D 下 FLEN 变成 64，`f0–f31` 每个寄存器存一个
double；f32 值共享同一组物理寄存器时按 NaN-boxing 规则存放。RVMin 不建模
NaN-boxing 的位级细节，但**同一个物理寄存器只能有一种宽度**这一点必须让
寄存器分配器知道——这就是 `FPR32` 与 `FPR64` 两个类共享 `F0–F31` 定义的原因。

ABI 仍然沿用第 40 章的 **B-lite** 策略：

- ABI 来源：`-target-abi` > IR 的 `target-abi` 模块标志 > ISA 推导
  （`+d → ilp32d`，`+f → ilp32f`，否则 `ilp32`）；
- `ilp32d` 必须有 D；`ilp32f` 不允许同时有 D（f64 合法后会走 `fa0–fa7`，
  那是 ILP32D 的约定，静默混用就是误编译）；
- `ilp32` 仍不接受 F/D（软 ABI + 硬指令的边界转换是第 43 章）。

## 代码地图

| 位置 | 职责 |
| --- | --- |
| `RVMin.td:67-72` | `FeatureStdExtD`（隐含 F）、`HasStdExtD` |
| `RVMin.td:88` | `-mcpu=generic-rv32-d` |
| `RVMinSubtarget.h:46,66` | `HasStdExtD` 字段与查询 |
| `RVMinRegisterInfo.td:141-150` | `FPR64`（**声明在 FPR32 之前**） |
| `RVMinInstrInfo.td:1110-1248` | 28 条 D 指令 |
| `RVMinInstrInfo.td:1251-1300` | f64 选择 pattern |
| `RVMinCallingConv.td:30-35,42` | f64 参数/返回值规则 |
| `RVMinISelLowering.cpp:63-64` | `addRegisterClass(MVT::f64, FPR64)` |
| `RVMinISelLowering.cpp:211-245` | f64 动作、扩展/截断动作 |
| `RVMinISelLowering.cpp:285-355` | ABI 解析与 `checkABICompatibility` |
| `RVMinISelLowering.cpp:631-676` | f64 `SELECT` 的栈实现 |
| `RVMinISelLowering.cpp:677-704` | f64 `ConstantFP` 的栈实现 |
| `RVMinInstrInfo.cpp:55-70` | FPR 拷贝（D 下用 `FSGNJ_D`） |
| `RVMinInstrInfo.cpp:255-300` | FPR64/FPR32/GPR 三路 spill/reload |
| `RVMinFrameLowering.cpp:267,291` | callee-saved 槽按寄存器类选 8/4/4 字节 |
| `RVMinISelDAGToDAG.cpp:67,85` | `FLD`/`FSD` 操作码 |
| `RVMinISelDAGToDAG.cpp:107-120` | 帧地址匹配同时接受 `ADD` 与 `OR` |
| `Disassembler/RVMinDisassembler.cpp:116-126` | `DecodeFPR64RegisterClass` |
| `clang/lib/Basic/Targets/RVMin.cpp:62,72,79,117-126` | CPU、扩展白名单、ABI 诊断 |
| `clang/lib/Driver/ToolChains/Arch/RISCV.cpp:100,432-434` | `rv32id`、`generic-rv32-d` |
| `llvm/test/MC/RVMin/d-extension.s` | 28 条编码 + 缺 D 诊断 + 反汇编 |
| `llvm/test/CodeGen/RVMin/d-ext.ll` | 代码生成、ABI 诊断、软浮点回归 |
| `scripts/rvmin-d-encoding-corpus.s` | 编码门禁语料 |
| `scripts/e2e/fp-double.c` | 端到端 double 程序 |

## 关键实现

### 1. D 隐含 F，FPR64 必须声明在 FPR32 之前

```tablegen
def FeatureStdExtD : SubtargetFeature<"d", "HasStdExtD", "true",
                                      "Double-precision floating point",
                                      [FeatureStdExtF]>;
```

`[FeatureStdExtF]` 让 `-mattr=+d` 自动打开 F，和 RISC-V 的 ISA 字符串规则一致
（`rv32id` 隐含 `f`）。

`FPR64` 与 `FPR32` 是同一组 `F0–F31` 的两个视图，分配顺序也完全相同：

```tablegen
def FPR64 : RegisterClass<"RVMin", [f64], 64,
  (add (sequence "F%u", 10, 17), (sequence "F%u", 0, 7),
       (sequence "F%u", 28, 31), F8, F9,
       (sequence "F%u", 18, 27))> {
  let DecoderMethod = "DecodeFPR64RegisterClass";
}
```

**声明顺序不是风格问题。** TableGen 生成的 `getMinimalPhysRegClass()` 对两个
互不包含的寄存器类返回“先声明且包含该寄存器”的那个；PEI（prologue/epilogue
insertion）用它决定 callee-saved 溢出的槽宽。如果 `FPR32` 在前，D 模式下
`fs0–fs11` 会拿到 4 字节槽，`fsd` 一写就越界。把 `FPR64` 放前面，D 模式下
槽宽统一按 8 字节，F 模式下仍然按 `FPR32` 的 4 字节（因为 f64 不合法，
`FPR64` 不会参与分配）。

### 2. 28 条指令：编码细节

指令分五组：

| 组 | 指令 |
| --- | --- |
| 访存 | `fld`, `fsd`（funct3 = `011`） |
| 算术 | `fadd.d`, `fsub.d`, `fmul.d`, `fdiv.d`, `fsqrt.d` |
| 符号/最值 | `fsgnj.d`, `fsgnjn.d`, `fsgnjx.d`, `fmin.d`, `fmax.d` |
| 比较 | `feq.d`, `flt.d`, `fle.d`, `fclass.d`（结果在 GPR） |
| 转换 | `fcvt.w.d`, `fcvt.wu.d`, `fcvt.d.w`, `fcvt.d.wu`, `fcvt.s.d`, `fcvt.d.s` |
| 融合乘加 | `fmadd.d`, `fmsub.d`, `fnmsub.d`, `fnmadd.d`（R4，fmt = `01`） |

舍入模式字段是最容易写错的地方：

| 指令 | `rm` | 原因 |
| --- | --- | --- |
| `fadd.d` 等普通运算 | `111` | DYN，运行时舍入模式 |
| `fcvt.w.d` / `fcvt.wu.d` | `111` | 默认 DYN |
| `fcvt.w.d ... rtz` / `fcvt.wu.d ... rtz` | `001` | C 的 `(int)d` 要求向零 |
| `fcvt.d.w` / `fcvt.d.wu` | `000` | 整数→double 永远精确，ISA 规定 RN |
| `fcvt.d.s` | `000` | float→double 永远精确，RN |
| `fcvt.s.d` | `111` | 可能丢精度，走 DYN |

`fcvt.d.s`/`fcvt.d.w` 用 `000` 而不是 `111` 是 RISC-V 的“legacy exact”
约定，参考汇编器就是这么编码的；门禁逐字节比较，不能凭直觉写。

`FCVT_W_D_RTZ` 与 `FCVT_W_D` 的差别只有 `rm` 与汇编文本：

```tablegen
def FCVT_W_D_RTZ : RVMinRInstFixedRS2<0b1100001, 0b001, 0b00000, OPC_OP_FP,
                                      (outs GPR:$rd), (ins FPR64:$rs1),
                                      "fcvt.w.d", "$rd, $rs1, rtz">;
```

这样 `fptosi` 的输出能被本汇编器重新汇编成相同字节（e2e 的 round-trip 要求）。

### 3. f64 成为合法类型

顺序与第 40 章相同，但 f64 必须在 `computeRegisterProperties` 之前注册：

```cpp
addRegisterClass(MVT::i32, &RVMin::GPRRegClass);
if (STI.hasStdExtF())
  addRegisterClass(MVT::f32, &RVMin::FPR32RegClass);
if (STI.hasStdExtD())
  addRegisterClass(MVT::f64, &RVMin::FPR64RegClass);
computeRegisterProperties(STI.getRegisterInfo());
```

动作表与 F 同形，只有一处例外——`ConstantFP` 不能走 bitcast：

```cpp
setOperationAction({ISD::FMINNUM, ISD::FMAXNUM}, MVT::f64, Legal);
setOperationAction(ISD::FREM, MVT::f64, LibCall);
setCondCodeAction(FPCCToExpand, MVT::f64, Expand);
setOperationAction(ISD::SELECT, MVT::f64, Custom);
setOperationAction(ISD::SELECT_CC, MVT::f64, Expand);
setOperationAction(ISD::BR_CC, MVT::f64, Expand);
setOperationAction(ISD::FP_EXTEND, MVT::f64, Legal);
setOperationAction(ISD::FP_ROUND, MVT::f32, Legal);
setOperationAction(ISD::ConstantFP, MVT::f64, Custom);
```

`FP_EXTEND/FP_ROUND` 合法化之后，`float ↔ double` 由 `FCVT_D_S`/`FCVT_S_D`
一条指令完成。

### 4. 常量与 select：RV32 只能过栈

RV32D 没有 `fmv.d.x`，而 `i64` 在本后端是非法的（没有 i64 寄存器类）。
因此 f64 的常量物化必须避开任何 i64 节点：

```cpp
// lowerConstantFP：8 字节私有栈槽 + 两条 i32 store + FLD
SDValue Slot = DAG.CreateStackTemporary(MVT::f64);
// 低 32 位 / 高 32 位分别 store，再 FLD
```

真实输出（`x * 1.5 + 2.0`）：

```asm
lui     a0, 262016
addi    a0, a0, 0
sw      a0, 4(sp)          # 1.5 的高 32 位
sw      zero, 0(sp)        # 低 32 位
fld     fa1, 0(sp)
fmul.d  fa0, fa0, fa1
sw      zero, 8(sp)        # 2.0 的高/低 32 位
sw      zero, 12(sp)
fld     fa1, 8(sp)
fadd.d  fa0, fa0, fa1
```

select 同理：两个 8 字节槽分别存两臂，把槽地址当整数做 mask select，最后
`fld` 选中那个地址：

```asm
fsd     fa0, 0(sp)
fsd     fa1, 8(sp)
addi    a1, sp, 0
addi    a2, sp, 8
xor     a2, a2, a1
sltiu   a0, a0, 1
sub     a0, zero, a0
and     a0, a2, a0
xor     a0, a0, a1
fld     fa0, 0(a0)
```

注意这里没有创建 i64 GPR 对，全部是合法类型（i32 + f64）。

### 5. ILP32D 调用约定与 callee-saved

CC 表新增 f64 规则，参数窗口与 f32 相同：

```tablegen
CCIfType<[f64], CCAssignToReg<[F10, F11, F12, F13, F14, F15, F16, F17]>>,
CCIfType<[f64], CCAssignToStack<8, 8>>,
CCIfType<[f64], CCAssignToReg<[F10, F11]>>    // RetCC
```

callee-saved 集合复用 `CSR_RVMin_FP`（`f8, f9, f18–f27`），F 与 D 共享。
区别只在 PEI 的槽宽：见“FPR64 声明顺序”。

`RVMinFrameLowering` 按寄存器实际所属的类选加载/存储指令：

```cpp
if (STI.hasStdExtD() && RVMin::FPR64RegClass.contains(CS.getReg()))
  RC = &RVMin::FPR64RegClass;   // 8 字节，fsd/fld
```

### 6. 拷贝、spill/reload 与栈访问

`copyPhysReg` 在 D 下用 `fsgnj.d` 拷贝 64 位（`fmv.d` 是它的别名）。查询必须
**动态**取函数自己的 subtarget：

```cpp
const bool HasD =
    MBB.getParent()->getSubtarget<RVMinSubtarget>().hasStdExtD();
```

`RVMinInstrInfo` 构造早于 `ParseSubtargetFeatures`，缓存特性标志会读到默认值。

spill/reload 有三路：

| 寄存器类 | 宽度 | store / load |
| --- | --- | --- |
| `FPR64` | 8 | `FSD` / `FLD` |
| `FPR32` | 4 | `FSW` / `FLW` |
| `GPR` | 4 | `SW` / `LW` |

栈访问选择器把 `getLoadOpcode/getStoreOpcode` 扩成 f64 → `FLD/FSD`，并把机器
节点的结果类型改成 `LD->getValueType(0)`。

还有一个 DAGCombine 细节：`FI + 4` 会被改写成 `FI | 4`，所以
`matchFrameAddr` 必须同时接受 `ISD::ADD` 与 `ISD::OR`。否则常量物化的
“两条 store + 一条 `fld`”会退化，甚至选不出指令。

### 7. ABI 校验与 clang

后端在 `LowerFormalArguments` 入口校验四种组合：

| ABI | 特性 | 结果 |
| --- | --- | --- |
| `ilp32` | 无 F | OK |
| `ilp32f` | F，无 D | OK |
| `ilp32d` | D | OK |
| `ilp32` | 有 F | 报错（第 43 章） |
| `ilp32f` | 有 D | 报错（f64 会误走 `fa0–fa7`） |
| `ilp32d` | 无 D | 报错 |

clang 侧在 `handleTargetFeatures` 做同样的检查，用户看到的是普通 `error:`：

```text
error: the ilp32f ABI with the D extension is not implemented; use -mabi=ilp32d or disable D
error: the ilp32d ABI requires the D extension
```

driver 的默认 ABI 推导把 `rv32id` 映射到 `ilp32d`；`-mcpu=generic-rv32-d`
映射到 `rv32id`。本章同时修正了第 40 章遗漏的一条：`generic-rv32-f → rv32if`
（此前 `-mcpu=generic-rv32-f` 会静默退回 `rv32i`）。

### 8. 两个“融合”陷阱：截断 store 与扩展 load

`float e = (float)d;` 在 DAG 里不是“`fptrunc` + `store`”，DAGCombine 会把它
融合成**截断 store**；反向的 `(double)f` 会融合成**扩展 load**：

```text
store<(store (s32) into %ir.e), trunc to f32>   ; f64 值写 f32 槽
load<(load (s32)), anyext from f32>             ; f32 槽读出 f64
```

generic 合法化默认认为这两件事“合法”，于是原样留给选择器；RVMin 只有
4/8 字节的普通 load/store pattern，选择阶段就崩在 `Cannot select`。
修复是把它们显式展开回两条 RVMin 认识的指令：

```cpp
setTruncStoreAction(MVT::f64, MVT::f32, Expand);   // -> FP_ROUND + FSW
setLoadExtAction(ISD::EXTLOAD, MVT::f64, MVT::f32, Expand); // -> FLW + FP_EXTEND
```

Expanded 之后 `FP_ROUND/FP_EXTEND` 是 Legal，自然选成 `FCVT_S_D/FCVT_D_S`。

## 构建与验证

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop

ninja -C "$B" llc clang llvm-mc llvm-lit

# 1) MC：28 条编码与参考汇编器逐字节一致
"$B/bin/llvm-mc" -triple=rvmin -mattr=+f,+d -show-encoding \
  "$W/llvm/lib/Target/RVMin/scripts/rvmin-d-encoding-corpus.s" > /tmp/rvmin-d.txt
"$B/bin/llvm-mc" -triple=riscv32 -mattr=+f,+d -show-encoding \
  "$W/llvm/lib/Target/RVMin/scripts/rvmin-d-encoding-corpus.s" > /tmp/riscv-d.txt
diff <(grep -o 'encoding: \[[^]]*\]' /tmp/rvmin-d.txt) \
     <(grep -o 'encoding: \[[^]]*\]' /tmp/riscv-d.txt)

# 2) 代码生成
cat > /tmp/d.c <<'C'
double dadd(double a, double b) { return a + b; }
int dtoi(double x) { return (int)x; }
double dmix(double x) { return x * 1.5 + 2.0; }
C
"$B/bin/clang" --target=rvmin-unknown-elf -march=rv32id -mabi=ilp32d -O2 \
  -S -emit-llvm /tmp/d.c -o /tmp/d.ll
"$B/bin/llc" -march=rvmin -mtriple=riscv32-unknown-elf -mattr=+f,+d \
  -O2 /tmp/d.ll -o -

# 3) 本章 lit
"$B/bin/llvm-lit" -v "$W/llvm/test/MC/RVMin/d-extension.s" \
                         "$W/llvm/test/CodeGen/RVMin/d-ext.ll"

# 4) 全部门禁（第 41 章时为 7 项；第 42 章加入 QEMU 后为 8 项）
bash "$W/llvm/lib/Target/RVMin/scripts/rvmin-verify-all.sh" "$B/bin"
```

## 预期输出

`dadd`、`dtoi` 与宽度转换：

```asm
dadd:
        fadd.d  fa0, fa0, fa1
        jalr    zero, 0(ra)

dtoi:
        fcvt.w.d a0, fa0, rtz
        jalr    zero, 0(ra)

narrow:
        fcvt.s.d fa0, fa0
        jalr    zero, 0(ra)

widen:
        fcvt.d.s fa0, fa0
        jalr    zero, 0(ra)
```

编码与反汇编门禁：

第 41 章提交时的门禁输出（当时 7 项）：

```text
PASS: 85 RV32I + 12 M + 19 Zbb + 8 Zbs + 20 A + 28 F + 28 D instructions, .text identical
PASS: d (28 instructions, .text round-trips byte-for-byte)
PASS: 173 encoding-corpus instructions + 6 label-resolved branches/jumps
```

e2e 与 clang 矩阵（12 程序 × 4 个 `-O` × 4 个特性集）：

```text
PASS: 12 programs x 0 1 2 3 optimization levels x 4 feature sets
      = 192 objects, every .text identical to its llvm-mc reassembly
PASS: e2e corpus under -march=rv32id -O2
ALL CLANG MATRIX PASS: 192 objects, ELF32 EM_RISCV, -S/-c agree,
      .text matches the riscv32 TargetInfo baseline
```

## 常见坑

1. **`FPR64` 必须声明在 `FPR32` 之前。** 否则 PEI 给 `fs0–fs11` 分 4 字节槽，
   `fsd` 溢出，运行前很难看出来；`-verify-machineinstrs` 也不一定抓得到。
2. **`RVMinInstrInfo` 不能缓存 `hasStdExtD()`。** 它构造于
   `ParseSubtargetFeatures` 之前；必须在 `copyPhysReg` 里通过
   `MBB.getParent()->getSubtarget<>()` 动态查询。
3. **“精确转换”用 `rm = 000`。** `fcvt.d.s`、`fcvt.d.w`、`fcvt.d.wu` 都是如此；
   写 `111` 会与参考汇编器差一个字节。
4. **f64 常量/select 不能走 i64。** `i64` 在本后端非法，op legalize 之后再
   引入 i64 节点会直接断言；必须用两个 i32 store + `fld`。
5. **`FI + C` 会变成 `FI | C`。** `matchFrameAddr` 只认 `ADD` 时，常量折叠
   后的栈地址匹配失败。
6. **`extload`/`truncstore` 默认被认为是合法的。** 不显式 Expand 就会在
   `conversions()` 这类“double 存 float 局部变量”的代码上 `Cannot select`。
7. **`-mabi=ilp32f -march=rv32id` 是合法 RISC-V 组合，但不是本章的组合。**
   psABI 规定它把 double 放在整型寄存器对里；RVMin 还没实现该边界转换，
   所以直接报错，而不是静默按 ILP32D 生成。
8. **`-mcpu=generic-rv32-f` 的 driver 映射容易漏。** 后端 CPU 列表、clang 的
   `getCPUSupported`、driver 的 `getMArchFromMcpu` 是三处独立白名单；漏一处
   不报错，只是静默退回 `rv32i`。
9. **`-target-abi` 必须与 IR 模块标志一致。** 与第 40 章相同，e2e 的 `+f,+d`
   单元要用 `clang -march=rv32id -mabi=ilp32d` 生成 IR。

## 练习

1. 实现 `ilp32f + D`：在 CC/形参/调用/返回四处把 f64 按 i32 对传递，函数体
   用栈把 i32 对拼回 f64。这是第 43 章“严格 ABI”的一半。
2. 给 `fclass.d` 加 intrinsic 与 pattern，覆盖 NaN/Inf/±0/次正规数，并与
   `fclass.s` 共用 lowering。
3. 把 f64 常量的物化优化成“两个 i32 常量 + `sw` 合并成一次 64 位栈写”，
   观察 `store` 合并 pass 是否会做，以及为什么 `i64` 非法让这件事变复杂。
4. 为 `dsel` 找一条不用栈的序列：提示 RV32D 没有 `fmv.d.x`，但可以用
   `feq.d` + 条件分支，或者用 `fsgnj.d` 构造 mask，比较代码大小与分支预测。
5. 把 `lowerConstantFP` 的栈槽提升到函数入口统一分配，测量对
   `-O0` 代码大小的影响，并解释为什么 PEI 不应该为每个常量单独开槽。

## 产物清单

- `llvm/lib/Target/RVMin/RVMin.td`：D 特性（隐含 F）、谓词、`generic-rv32-d`
- `RVMinRegisterInfo.td`：`FPR64`（声明在 `FPR32` 之前）
- `RVMinInstrInfo.td`：28 条 D 指令 + f64 选择 pattern
- `RVMinCallingConv.td`：f64 参数/返回值规则
- `RVMinSubtarget.h`：`HasStdExtD`
- `RVMinISelLowering.h/.cpp`：f64 注册、动作、ABI 校验、常量/select
- `RVMinInstrInfo.cpp`：FPR 拷贝、三路 spill/reload
- `RVMinFrameLowering.cpp`：按寄存器类选 8/4 字节 callee-saved 槽
- `RVMinISelDAGToDAG.cpp`：`FLD/FSD`、`ADD|OR` 帧地址
- `Disassembler/RVMinDisassembler.cpp`：`DecodeFPR64RegisterClass`
- `clang/lib/Basic/Targets/RVMin.cpp`：`generic-rv32-d`、`d`、ABI 诊断
- `clang/lib/Driver/ToolChains/Arch/RISCV.cpp`：`rv32id`、`generic-rv32-f/d`
- `llvm/test/MC/RVMin/d-extension.s`、`llvm/test/CodeGen/RVMin/d-ext.ll`
- `scripts/rvmin-d-encoding-corpus.s`、`scripts/e2e/fp-double.c`
- `scripts/check-rvmin-encodings.sh`、`check-rvmin-disasm.sh`、`rvmin-e2e.sh`、
  `rvmin-clang-matrix.sh`
- `docs/29`–`docs/39` 的 GENERATED TD USAGE 行号刷新
