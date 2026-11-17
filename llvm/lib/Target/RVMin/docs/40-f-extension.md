# 第 40 章：F 扩展与 ILP32F 调用约定

## 目标

前 39 章里，`float` 是一个“被软化的类型”：没有 f32 寄存器类，类型合法化把每
个浮点值拆成 i32 对，每条算术都变成 `__addsf3` 之类的 libcall。本章打开 RISC-V
的 **F 扩展**，完成之后：

1. `-mattr=+f` 下 `fadd/fsub/fmul/fdiv/fsqrt` 各一条指令；
2. `fabs/fneg/fcopysign` 分别选成 `fsgnjx/fsgnjn/fsgnj`，`fmin/fmax` 选成
   `fmin.s/fmax.s`，`fma` 选成 `fmadd.s`；
3. `fcvt.w.s`/`fcvt.wu.s`（RTZ）、`fcvt.s.w`/`fcvt.s.wu`、`feq/flt/fle/fclass`
   以及 `fmv.x.w`/`fmv.w.x` 全部可用；
4. f32 形参走 `fa0–fa7`、f32 返回值走 `fa0`，`fs0–fs11` 成为 callee-saved；
5. `clang --target=rvmin-unknown-elf -march=rv32if` 一条命令端到端可用；
6. 不开 F 时输出与第 39 章**逐字节一致**（软浮点 libcall 路径不变）；
7. 28 条指令的编码与参考 `llvm-mc -triple=riscv32 -mattr=+f` 逐字节相同。

本章只做单精度。D 扩展（f64、FPR64、ILP32D）在第 41 章，第四篇验收在第 42 章。

## 背景：特性与 ABI 是两件事

RISC-V 把“有哪些指令”和“值怎么跨函数传递”分开描述：

| | 指令选择 | 调用约定 |
| --- | --- | --- |
| 由谁决定 | ISA 特性（`-mattr=+f`） | ABI（`-mabi=ilp32f`） |
| f32 在函数内部 | FPR32 + `fadd.s` | — |
| f32 跨函数边界 | — | `fa0–fa7` |

真实工具链允许四种组合，例如 `-march=rv32if -mabi=ilp32` 表示“内部用硬浮点
指令，跨函数仍按软浮点 ABI 传 i32 位模式”。这种组合需要在调用边界做
`fmv.w.x`/`fmv.x.w` 转换，是第 51 章的练习。

本章采用 **B-lite**：

- ABI 从 `-target-abi` > IR 的 `target-abi` 模块标志 > ISA 推导
  （`+f → ilp32f`，否则 `ilp32`）；
- 合法组合只有 `ilp32`（无 F）和 `ilp32f`（有 F，无 D）；
- `ilp32 + F`、`ilp32f` 无 F、`ilp32d`（D 还没实现）都在 lowering 入口报错，
  不静默生成 ABI 不匹配的代码；
- 因此 `llc -mattr=+f` 手写 IR 开箱即用，clang 产生的 IR 则按模块标志执行。

## 代码地图

| 位置 | 职责 |
| --- | --- |
| `RVMin.td:58-63` | `FeatureStdExtF`、`HasStdExtF` |
| `RVMin.td:77` | `-mcpu=generic-rv32-f` |
| `RVMinSubtarget.h:44,63` | `HasStdExtF` 字段与查询 |
| `RVMinRegisterInfo.td:83-140` | `F0–F31`、ABI 别名、`FPR32` 寄存器类 |
| `RVMinInstrFormats.td:55,75-81,252,279` | R4 格式、FP 操作码、R4/固定 rs2 指令类 |
| `RVMinInstrInfo.td:916-1110` | 28 条指令、选择 pattern、内存 pattern |
| `RVMinCallingConv.td` | f32 参数/返回值规则、`CSR_RVMin_FP` |
| `RVMinRegisterInfo.cpp` | callee-saved 与 call-preserved mask 按特性分派 |
| `RVMinInstrInfo.cpp` | `copyPhysReg` 的 FPR 三分支、FPR spill/reload |
| `RVMinFrameLowering.cpp` | callee-saved FPR 用 `fsw`/`flw` |
| `RVMinISelLowering.cpp:52,180-200,234,250-300,571,633` | FPR 注册、FP 动作、ABI、select、ABI 校验 |
| `RVMinISelDAGToDAG.cpp:58-95,129,185-215` | `FLW/FSW`、f32 常量、栈访问结果类型 |
| `Disassembler/RVMinDisassembler.cpp:97-112` | FPR 解码表 |
| `AsmParser/RVMinAsmParser.cpp:358-380` | `fcvt.*` 的 `rtz` token |
| `clang/lib/Basic/Targets/RVMin.cpp:49,61,71,77,131` | `setABI`、CPU、扩展白名单 |
| `clang/lib/Driver/ToolChains/Arch/RISCV.cpp:109,269` | `rv32if → ilp32f`、`+f` 直达汇编器 |
| `llvm/test/MC/RVMin/f-extension.s` | 28 条编码 + 缺特性诊断 + 反汇编 |
| `llvm/test/CodeGen/RVMin/f-ext.ll` | 代码生成、软浮点回归、ABI 诊断 |
| `scripts/rvmin-f-encoding-corpus.s` | 编码门禁语料 |
| `scripts/e2e/fp.c` | 端到端浮点程序 |

## 关键实现

### 1. 特性三件套与 FPR32

```tablegen
def FeatureStdExtF : SubtargetFeature<"f", "HasStdExtF", "true",
                                      "Single-precision floating point">;
def HasStdExtF : Predicate<"Subtarget->hasStdExtF()">,
                 AssemblerPredicate<(all_of FeatureStdExtF),
                                    "'F' (Single-Precision Floating-Point)">;
```

寄存器文件与整数文件分离。`F0–F31` 的 HWEncoding 是 0–31，DWARF 号是 32–63；
ABI 别名按标准给出（`ft0–ft7 / fs0–fs1 / fa0–fa7 / fs2–fs11 / ft8–ft11`）。
`FPR32` 的分配顺序把调用者保存的参数寄存器放前面，callee-saved 的 `fs*` 放最后：

```tablegen
def FPR32 : RegisterClass<"RVMin", [f32], 32,
  (add (sequence "F%u", 10, 17), (sequence "F%u", 0, 7),
       (sequence "F%u", 28, 31), F8, F9, (sequence "F%u", 18, 27))> {
  let DecoderMethod = "DecodeFPR32RegisterClass";
}
```

### 2. 指令格式：R4 与固定 rs2

F 引入两种新形状：

- **R4**：`fmadd.s/fmsub.s/fnmsub.s/fnmadd.s`，多一个 `rs3`，`fmt=00` 表示
  单精度，`rm` 位固定为 `111`（DYN）；
- **固定 rs2 的 R 型**：`fsqrt.s/fcvt.*/fmv.*/fclass.s`，`rs2` 字段是操作码的
  一部分，定义为 `RVMinRInstFixedRS2`。

`FCVT_W_S_RTZ` 与普通 `FCVT_W_S` 的编码只差 `rm` 字段（001 vs 111），但汇编
文本必须区分：`fptosi` 用 RTZ 版本，打印成 `fcvt.w.s a0, fa0, rtz`，这样 llc
的输出能被本汇编器重新汇编成相同字节。

### 3. f32 成为合法类型

关键是顺序：

```cpp
addRegisterClass(MVT::i32, &RVMin::GPRRegClass);
if (STI.hasStdExtF())
  addRegisterClass(MVT::f32, &RVMin::FPR32RegClass);
computeRegisterProperties(STI.getRegisterInfo());
```

`computeRegisterProperties` 根据寄存器类推导类型动作；放在它之后加 f32 会被忽略，
f32 仍然被软化。加好之后，只需把“默认 Expand 但 F 有单条指令”的操作改回来：

- `FMINNUM/FMAXNUM` → `Legal`；
- 只保留 `SETOEQ/SETOLT/SETOLE` 三个比较条件，其余条件 `Expand` 成
  “三者之一 + XOR/交换”；
- `SELECT/SELECT_CC/BR_CC` 在 f32 上没有指令，但 `SELECT` 必须 `Custom`
  （见“常见坑 3”）；
- `FREM` 与超越函数走 libcall。

### 4. 常量：不建常数池

`SelectionDAGBuilder` 只在 `isFPImmLegal` 为真时保留 `ConstantFP`，否则生成
`ConstantPool`。本后端把 `getFPImmAction` 覆盖成 `Bitcast`：

```cpp
RVMinTargetLowering::FPImmAction
RVMinTargetLowering::getFPImmAction(const APFloat &, EVT VT, bool) const {
  if (VT == MVT::f32 && isTypeLegal(VT))
    return FPImmAction::Bitcast;
  return FPImmAction::ConstantPool;
}
```

合法化器于是生成“不透明 i32 常量 + BITCAST”。选择器把常量按
`LUI+ADDI` 物化，再用 `FMV_W_X` 移到 FPR：

```asm
lui a0, 261120        # 1.5f = 0x3fc00000
addi a0, a0, 0
fmv.w.x fa0, a0
```

### 5. 调用约定与 callee-saved

CC 表新增两条规则（参数、栈），返回表新增一条：

```tablegen
CCIfType<[f32], CCAssignToReg<[F10..F17]>>,
CCIfType<[f32], CCAssignToStack<4, 4>>,
CCIfType<[f32], CCAssignToReg<[F10, F11]>>   // RetCC
```

这些规则**不需要**按特性加谓词：不开 F 时，f32 在到达 CC 分析之前已经被
`getRegisterTypeForCallingConv` 软化成了 i32，f32 规则永远不会被查询。

callee-saved 列表在整数集合上追加 `f8, f9, f18–f27`：

```tablegen
def CSR_RVMin_FP : CalleeSavedRegs<(add CSR_RVMin, F8, F9,
                                       (sequence "F%u", 18, 27))>;
```

`RVMinRegisterInfo` 按 `hasStdExtF()` 返回 `CSR_RVMin_FP_SaveList/RegMask`；
`RVMinFrameLowering` 按寄存器所属类选 `fsw/flw` 或 `sw/lw`。

### 6. 形参、调用与栈访问

形参 lowering 从硬编码 GPR 改成由位置类型决定：

```cpp
Register VReg = RegInfo.createVirtualRegister(getRegClassFor(LocVT));
```

栈访问选择器为 f32 增加两个助记符：

- `getLoadOpcode`：`memoryVT == f32 && NON_EXTLOAD` → `FLW`；
- `getStoreOpcode`：`memoryVT == f32 && !truncating` → `FSW`；
- 机器节点的结果类型从固定的 `MVT::i32` 改成 `LD->getValueType(0)`。

### 7. ABI 解析与校验

```cpp
RVMinABI RVMinTargetLowering::getABI(const MachineFunction &MF) const {
  StringRef Name = getTargetMachine().getTargetABIName(*MF.getFunction().getParent());
  if (Name.empty())
    return MF.getSubtarget<RVMinSubtarget>().hasStdExtF() ? RVMinABI::ILP32F
                                                           : RVMinABI::ILP32;
  ...
}
```

`getTargetABIName` 已经实现了“模块标志优先，否则 `-target-abi`”。校验放在
`LowerFormalArguments` 入口：

- `ilp32 + F`：报错，提示改用 `-mabi=ilp32f`；
- `ilp32f` 无 F：报错；
- `ilp32d`：报错（第 41 章实现）；
- 未知 ABI：报错。

clang 侧在 `handleTargetFeatures` 里也做同样检查，这样用户看到的是普通
`error:`，而不是后端 fatal。

### 8. 两个必须单独处理的 f32 控制流

- **分支**：`lowerBRCOND` 只有在 SETCC 的操作数是 i32 时才把比较“压进”分支。
  f32 比较必须先生成 `feq.s/flt.s`（结果是 GPR 0/1），再 `bne` 这个 GPR；
  否则会留下无法选择的 f32 `BR_CC`。
- **选择**：`select c, t, f` 把两个分支 bitcast 到 i32，复用整数 select 的
  mask 序列，再 bitcast 回 f32。

## 构建与验证

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop

ninja -C "$B" llc clang llvm-mc llvm-objdump llvm-lit

# 1) MC：28 条编码与参考汇编器逐字节一致
"$B/bin/llvm-mc" -triple=rvmin -mattr=+f -show-encoding \
  "$W/llvm/lib/Target/RVMin/scripts/rvmin-f-encoding-corpus.s" > /tmp/rvmin-f.txt
"$B/bin/llvm-mc" -triple=riscv32 -mattr=+f -show-encoding \
  "$W/llvm/lib/Target/RVMin/scripts/rvmin-f-encoding-corpus.s" > /tmp/riscv-f.txt
diff <(grep -o 'encoding: \[[^]]*\]' /tmp/rvmin-f.txt) \
     <(grep -o 'encoding: \[[^]]*\]' /tmp/riscv-f.txt)

# 2) 代码生成
cat > /tmp/f.c <<'C'
float fadd(float a, float b) { return a + b; }
int ftoi(float x) { return (int)x; }
float fmix(float x) { return x * 1.5f + 2.0f; }
C
"$B/bin/clang" --target=riscv32-unknown-elf -march=rv32if -mabi=ilp32f -O2 \
  -S -emit-llvm /tmp/f.c -o /tmp/f.ll
"$B/bin/llc" -march=rvmin -mtriple=riscv32-unknown-elf -mattr=+f -O2 /tmp/f.ll -o -

# 3) 本章 lit
"$B/bin/llvm-lit" -v "$W/llvm/test/MC/RVMin/f-extension.s" \
                         "$W/llvm/test/CodeGen/RVMin/f-ext.ll"

# 4) 全部门禁（7 项）
bash "$W/llvm/lib/Target/RVMin/scripts/rvmin-verify-all.sh" "$B/bin"
```

## 预期输出

`fadd` 与 `ftoi`：

```asm
fadd:
        fadd.s  fa0, fa0, fa1
        jalr    zero, 0(ra)

ftoi:
        fcvt.w.s a0, fa0, rtz
        jalr    zero, 0(ra)
```

`fmix`（常量 1.5f 与 2.0f 都不进常数池）：

```asm
fmix:
        lui     a0, 261120          # 0x3fc00000 = 1.5f
        addi    a0, a0, 0
        fmv.w.x fa1, a0
        fmul.s  fa0, fa0, fa1
        lui     a0, 262144          # 0x40000000 = 2.0f
        addi    a0, a0, 0
        fmv.w.x fa1, a0
        fadd.s  fa0, fa0, fa1
        jalr    zero, 0(ra)
```

门禁的预期行：

```text
PASS: 85 RV32I + 12 M + 19 Zbb + 8 Zbs + 20 A + 28 F instructions, .text identical
PASS: f (28 instructions, .text round-trips byte-for-byte)
PASS: 145 encoding-corpus instructions + 6 label-resolved branches/jumps
PASS: 11 programs x 0 1 2 3 optimization levels x 3 feature sets
      = 132 objects, every .text identical to its llvm-mc reassembly
PASS 7/7: TableGen DSL contracts + status matrix + field effect
ALL GATES PASS (run rvmin-run.sh separately for the QEMU check)
```

clang 矩阵多出 `rv32if` 一行：

```text
PASS: e2e corpus under -march=rv32if -O2
ALL CLANG MATRIX PASS: 132 objects, ELF32 EM_RISCV, -S/-c agree,
      .text matches the riscv32 TargetInfo baseline
```

## 常见坑

1. **寄存器类必须在 `computeRegisterProperties` 之前注册。** 顺序反了不会报错，
   只是 f32 继续被软化，`-mattr=+f` 看起来“没生效”。
2. **`isFPImmLegal` 不是虚函数。** 要覆盖的是 `getFPImmAction`；返回
   `FPImmAction::Bitcast` 才会得到“不透明 i32 常量 + BITCAST”，也才不会把
   BITCAST 折叠回 `ConstantFP`。
3. **`SELECT` 与 `SELECT_CC` 不能同时 Expand。** legalizer 会断言
   `Cannot expand ISD::SELECT_CC when ISD::SELECT also needs to be expanded`。
   f32 的 `SELECT` 用 `Custom`，在 lowering 里转成 i32 select。
4. **f32 的 `BR_CC` 无法选择。** F 没有浮点条件分支；`lowerBRCOND` 必须把
   f32 比较留在 GPR 里，再对 0/1 结果 `bne`。
5. **`rtz` 是 token，不是符号。** 汇编器的通用路径会把裸标识符解析成符号表达式，
   于是 `fcvt.w.s a0, fa0, rtz` 匹配失败。`parseOperand` 对 `fcvt.*` 特判，
   产出 `Token` 操作数，才能和 AsmString 里的字面量比较。
6. **`-target-abi` 必须与 IR 模块标志一致。** `TargetMachine::verifyOptionsConsistency`
   会在两者不一致时报错，所以 e2e 的 `+f` 单元要用 `clang -march=rv32if -mabi=ilp32f`
   生成 IR，而不是给 llc 强塞 `-target-abi`。
7. **`fabs`/`fmv.s` 是别名。** `fabs.s fa0, fa0` 与 `fsgnjx.s fa0, fa0, fa0`
   是同一个编码；lit 断言要按自己生成的助记符写。
8. **callee-saved 恢复不能硬编码 `GPRRegClass`。** 否则 `fs0–fs11` 会走 `sw/lw`，
   在机器码验证或运行时才暴露。
9. **软浮点回归要看“没有 F 指令”，而不只是“能编译”。** 不开 F 时 clang 的 IR
   仍是 `fadd float`，libcall 是后端 legalizer 产生的；e2e 同时编译和汇编，
   能抓住 ABI 标志不一致导致的静默错误。

## 练习

1. 实现 `ilp32 + F`（第 51 章预告）：在 CC 表里对 f32 做
   `CCBitConvertToType<i32>`，并在形参/调用/返回三处插 `BITCAST`。先只做 f32，
   再考虑 f64 的 i32 对。
2. 给 `fclass.s` 加一条 intrinsic（`llvm.riscv.fclass` 风格）和 pattern，写一个
   测试覆盖 NaN/Inf/±0/次正规数。
3. 把舍入模式做成真正的操作数：`fcvt.w.s a0, fa0, rne/rtz/rdn/rup/rmm/dyn`，
   像上游 `FRMArg` 那样自定义 parser/printer/decoder。
4. 用 `-mattr=+f` 编译一个含 `float` 结构体参数/返回值的程序，观察 ABI 如何处理
   “两个 f32 占一个参数槽”的规则，并与 `llvm-mc -triple=riscv32` 的期望对照。
5. 把 `fabsneg` 的 `fabs(fneg(x))` 组合优化成单条 `fsgnjx`，确认 DAG combine
   已经这么做了，再找一个新的 FP 组合优化（例如 `fneg(fneg(x))`）。

## 产物清单

- `llvm/lib/Target/RVMin/RVMin.td`：F 特性、谓词、处理器
- `RVMinRegisterInfo.td`：FPR 定义、`FPR32` 类
- `RVMinInstrFormats.td`：R4、FP 操作码、两个新指令类
- `RVMinInstrInfo.td`：28 条指令 + 选择 pattern
- `RVMinCallingConv.td`：f32 CC 规则、`CSR_RVMin_FP`
- `RVMinSubtarget.h/.cpp`：`HasStdExtF`
- `RVMinRegisterInfo.cpp`、`RVMinInstrInfo.cpp`、`RVMinFrameLowering.cpp`：
  FPR 的 copy/spill/callee-saved
- `RVMinISelLowering.h/.cpp`：FPR 注册、FP 动作、ABI、select/brcond
- `RVMinISelDAGToDAG.cpp`：`FLW/FSW`、常量物化
- `Disassembler/RVMinDisassembler.cpp`：FPR 解码
- `AsmParser/RVMinAsmParser.cpp`：`rtz` token
- `clang/lib/Basic/Targets/RVMin.cpp`、`clang/lib/Driver/ToolChains/Arch/RISCV.cpp`
- `llvm/test/MC/RVMin/f-extension.s`、`llvm/test/CodeGen/RVMin/f-ext.ll`
- `scripts/rvmin-f-encoding-corpus.s`、`scripts/e2e/fp.c`
- `scripts/check-rvmin-encodings.sh`、`check-rvmin-disasm.sh`、`rvmin-e2e.sh`、
  `rvmin-clang-matrix.sh`
- `docs/29`–`docs/39` 的 GENERATED TD USAGE 行号刷新
