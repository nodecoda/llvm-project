# 第 55 章：LMUL=1 向量 ABI——v8–v23 的调用边界

## 目标

第 54 章让 `v4f32` 在寄存器内可用，但向量值还不能跨函数调用。本章把
**LMUL=1 的整数向量**接进 RVMin 的调用约定：

- `<4 x i32>` 参数按 **v8–v23 共 16 个**寄存器传递；
- 第 **17** 个及以后的向量参数进栈，栈槽 16 字节大小、16 字节对齐；
- `<4 x i32>` 返回值放 v8；
- v1–v7、v24–v31 作为向量 callee-saved；
- 同时启用 `+v,+f` 时，浮点 callee-saved（fs0–fs11）与向量 callee-saved
  同时生效；
- 删除第 48 章遗留的 blanket vector ABI 拒绝，但保留明确的未支持诊断。

本章**不做**：

- LMUL=2 的跨调用 ABI（`<8 x i32>` 参数/返回）；
- `v4f32` 的跨调用 ABI；
- varargs 中的向量参数、多个向量返回值、sret/byval 向量；
- 完整的 `riscv_vector_cc` 属性链。

> 这是 RVMin 的教学口径：在默认 `CC_RVMin` / `RetCC_RVMin` 里实现一个
> 可验证的最小向量调用边界，不是上游 RISC-V 后端的完整 vector CC。

---

## 背景：为什么是 v8–v23，为什么是 16 个

RISC-V 向量调用约定把向量寄存器分成三组：

```text
v0          : mask 寄存器，永远不作为普通参数
v8  – v23   : 向量参数/返回值窗口（16 个）
v1–v7       : 向量 callee-saved（低区）
v24 – v31   : 向量 callee-saved（高区）
```

本章的 VLEN 是 128 位，一个 `<4 x i32>` 正好占满一个 v 寄存器。
因此“前 16 个向量参数进 v8–v23”意味着：

```text
参数 1..16  ->  v8, v9, ..., v23
参数 17     ->  stack + 0
参数 18     ->  stack + 16
...
```

每个栈槽 16 字节，按 16 字节对齐。这个窗口不是 8 个，也不是“前 8 个在寄存器、
第 9 个在栈”；第 48 章的计划里曾把窗口写小，本章以 **16** 为准。

v0 是架构 mask。第 52 章的 `vmerge.vvm`、masked ALU 都把它当掩码；
所以 `CC_RVMin_V` 的参数寄存器列表从 v8 开始，绝不包含 v0。

---

## 设计

### 1. feature-gated 的向量子表

`llvm/lib/Target/RVMin/RVMinCallingConv.td:52-61`：

```tablegen
def CC_RVMin_V : CallingConv<[
  CCIfType<[v4i32], CCAssignToReg<[V8, V9, V10, V11, V12, V13, V14, V15,
                                  V16, V17, V18, V19, V20, V21, V22, V23]>>,
  CCIfType<[v4i32], CCAssignToStack<16, 16>>
]>;

def RetCC_RVMin_V : CallingConv<[
  CCIfType<[v4i32], CCAssignToReg<[V8]>>
]>;
```

然后在默认表里委托：

```tablegen
CCIf<"RVMinHasStdExtV(State)", CCDelegateTo<CC_RVMin_V>>,
```

`RetCC_RVMin` 同理委托到 `RetCC_RVMin_V`。`RVMinHasStdExtV` 定义在
`RVMinISelLowering.cpp:68-71`，通过 `CCState` 拿到当前 `MachineFunction` 的
subtarget。

**为什么必须 feature-gate**：没有 V 时，类型合法化会把 `<4 x i32>` 拆成
标量；如果无条件把 `v4i32` 分配到 V8，`rv32i`/`+m`/`+c` 的门禁 6 会立刻失败。
委托条件保证只有 `hasStdExtV()` 时这条规则才可达。

### 2. 返回值

`RetCC_RVMin_V` 只把 `v4i32` 放到 v8。调用方在 `LowerCall` 里通过
`CCInfo.AnalyzeCallResult(Ins, RetCC_RVMin)` 拿到 `VA.getLocReg() == V8`，
再用 `CopyFromReg` 取回向量值。

### 3. callee-saved：四选一

`RVMinCallingConv.td:121-128`：

```tablegen
def CSR_RVMin_V : CalleeSavedRegs<(add CSR_RVMin,
                                   (sequence "V%u", 1, 7),
                                   (sequence "V%u", 24, 31))>;

def CSR_RVMin_FP_V : CalleeSavedRegs<(add CSR_RVMin_FP,
                                      (sequence "V%u", 1, 7),
                                      (sequence "V%u", 24, 31))>;
```

`RVMinRegisterInfo.cpp:43-68` 把原来的二选一改成四选一：

```cpp
if (HasV && HasF) return CSR_RVMin_FP_V_SaveList;
if (HasV)         return CSR_RVMin_V_SaveList;
if (HasF)         return CSR_RVMin_FP_SaveList;
return CSR_RVMin_SaveList;
```

`getCallPreservedMask` 使用完全相同的顺序。这样：

- `+v`：向量 callee-saved 生效，FPR 不在 preserved mask 里；
- `+f`：FPR callee-saved 生效，向量不在；
- `+v,+f`：两者的并集；
- 都没有：原来的整数 `CSR_RVMin`。

### 4. 类型白名单与诊断

`RVMinISelLowering.cpp:74-89` 定义三个谓词：

```cpp
static bool isSupportedVectorABIType(EVT VT) { return VT == MVT::v4i32; }
static bool isLMUL2VectorABIType(EVT VT) { return VT == MVT::v8i32; }
```

IR 层再各有一个 `const Type *` 版本，用来在 `LowerFormalArguments` /
`LowerCall` / `CanLowerReturn` 里做同样的判断。

规则：

| 类型 | `+v` 时 | 诊断 |
| --- | --- | --- |
| `<4 x i32>` | 允许 | 无 |
| `<8 x i32>` | 拒绝 | `RVMin: LMUL=2 vector arguments are not supported yet`（返回路径是 `... returns ...`） |
| `<4 x float>` | 拒绝 | `RVMin: vector arguments are not supported yet` |
| `<4 x i1>` | 拒绝 | 同上 |

`v8i32` 需要偶数对齐的寄存器对，`CCAssignToReg` 不能表达“v24/v25 作为一对”，
所以本章明确拒绝，而不是把它塞进一个任意的 vN。

### 5. CFI 边界：向量没有 DWARF 编号

`RVMinFrameLowering.cpp:198-212` 的 `emitPrologueCFI` 和
`RVMinFrameLowering.cpp:228-236` 的 `emitEpilogue` 都有一个跳过：

```cpp
// RVMin's vector registers have no DWARF register numbers yet.  Emitting
// buildOffset() for one would produce `.cfi_offset 4294967295`, which is
// not a valid unwind rule.  The vector callee-saved spill still happens;
// only the CFI entry is omitted.
if (RVMin::VRRegClass.contains(CS.getReg()))
  continue;
```

向量 spill 仍然发生，只是不生成 unwinder 规则。这是本章有意保留的缺口：
等 RVMin 定义了 V 寄存器的 DWARF 编号，再把这两处跳过删掉。

### 6. 没有 V 时

`LowerFormalArguments` / `LowerCall` 的向量检查都包在
`if (STI.hasStdExtV())` 里；`CanLowerReturn` 在没有 V 时对向量返回
`return false`，让通用 sret demotion 处理。这样门禁 6 用
`-mattr=""`、`+m`、`+c` 跑同一份正例时不会触发 fatal error。

---

## 代码地图

| 文件 | 作用 |
| --- | --- |
| `RVMinCallingConv.td:52-61` | `CC_RVMin_V` / `RetCC_RVMin_V` |
| `RVMinCallingConv.td:69`、`:90` | feature-gated 委托点 |
| `RVMinCallingConv.td:121-128` | `CSR_RVMin_V` / `CSR_RVMin_FP_V` |
| `RVMinRegisterInfo.cpp:43-68` | CSR 四选一 |
| `RVMinFrameLowering.cpp:198-212`、`:245-310` | VR spill/restore、CFI 跳过 |
| `RVMinISelLowering.cpp:68-89` | `RVMinHasStdExtV` 与类型谓词 |
| `RVMinISelLowering.cpp:1156-1160` | 形参检查 |
| `RVMinISelLowering.cpp:1269-1288` | 调用参数/结果检查 |
| `RVMinISelLowering.cpp:1473-1490`、`:1513-1516` | 返回检查 |
| `RVMinInstrInfo.cpp:291-350` | 已有的 `VR`/`VRM2` spill/reload |
| `test/CodeGen/RVMin/v-ext-abi-arg.ll` | v8 参数、第 17 个栈参数 |
| `test/CodeGen/RVMin/v-ext-abi-ret.ll` | v8 返回 |
| `test/CodeGen/RVMin/v-abi.ll` | 16 个参数、callee-saved、m2、`+v,+f` |
| `test/CodeGen/RVMin/v-abi-m2.ll` | `v8i32` 的 LMUL=2 负例 |

---

## 关键实现

### 1. 向量参数进入 v8

```llvm
define void @vec_arg(ptr %p, <4 x i32> %v) {
  store <4 x i32> %v, ptr %p, align 16
  ret void
}
```

`-O2 -mattr=+v`：

```asm
vec_arg:
  vsetivli zero, 4, e32, m1, ta, ma
  vse32.v v8, (a0)
  jalr zero, 0(ra)
```

`v8` 就是第一个向量参数，`a0` 是第一个整数参数（`ptr`）。

### 2. 第 17 个参数在栈上

```llvm
define <4 x i32> @arg17(<4 x i32> %a0, ..., <4 x i32> %a16) {
  ret <4 x i32> %a16
}
```

`-O2 -mattr=+v`：

```asm
arg17:
  addi a0, sp, 0
  vsetivli zero, 4, e32, m1, ta, ma
  vle32.v v8, (a0)
  jalr zero, 0(ra)
```

前 16 个参数在 v8–v23，第 17 个在 `0(sp)`；callee 从栈加载到 v8 再返回。

调用方 `call17` 则把第 17 个参数写到 outgoing area：

```asm
addi sp, sp, -32
sw ra, 28(sp)
addi a0, sp, 32
vsetivli zero, 4, e32, m1, ta, ma
vle32.v v0, (a0)
vse32.v v0, (sp)
jalr ra, 0(t0)
```

这里 `v0` 只是寄存器分配器选中的临时寄存器；调用点没有活跃的 mask，
所以用 v0 搬运第 17 个参数不会破坏任何 masked 指令。

### 3. 返回值在 v8

```llvm
define <4 x i32> @vec_ret(ptr %p) {
  %v = load <4 x i32>, ptr %p, align 16
  ret <4 x i32> %v
}
```

```asm
vec_ret:
  vsetivli zero, 4, e32, m1, ta, ma
  vle32.v v8, (a0)
  jalr zero, 0(ra)
```

`LowerReturn` 不需要特殊分支：`RetCC_RVMin_V` 把位置分配到 V8，
通用的 `CopyToReg` 就完成了。

### 4. callee-saved 高压力

`v-abi.ll` 的 `callee_saved` 让 20 个 `<4 x i32>` 同时跨 `@bar()` 活跃。
寄存器分配器会用满 v24–v31，prologue/epilogue 出现：

```asm
addi sp, sp, -32
sw ra, 28(sp)
sw s0, 24(sp)
vsetivli zero, 4, e32, m1, ta, ma
vse32.v v24, (sp)
...
vle32.v v24, (sp)
```

v1–v7 与 v24–v31 都在 `CSR_RVMin_V` 里，PEI 只在函数真正修改它们时
才创建栈槽。

### 5. `+v,+f`：两套 callee-saved 同时生效

`v-abi.ll` 的 `fp_v_csr` 让 12 个 float 和 20 个 `<4 x i32>` 跨调用活跃。
`-O2 -mattr=+v,+f -target-abi=ilp32f`：

```asm
fsw fs0, 96(sp)
vsetivli zero, 4, e32, m1, ta, ma
vse32.v v24, (a2)
vse32.v v25, (a2)
...
vse32.v v29, (sp)
...
flw fs0, 96(sp)
vle32.v v24, (a0)
```

同一 prologue 同时保存 FPR 与 VR，证明 `CSR_RVMin_FP_V` 生效。

### 6. LMUL=2 的 callee-saved：两半都要保存

`v-abi.ll` 的 `m2_csr` 让 20 个 `<8 x i32>` 跨调用活跃。`-O2 -mattr=+v`：

```asm
vsetivli zero, 4, e32, m1, ta, ma
vse32.v v24, (a2)
vse32.v v25, (sp)
...
vle32.v v24, (a0)
vle32.v v25, (sp)
```

一个 `<8 x i32>` 的值在机器层跨 V24/V25。PEI 的
`MachineRegisterInfo::isPhysRegModified` 是 alias-aware 的
（`llvm/lib/CodeGen/MachineRegisterInfo.cpp:594-608`），
`V24M2` 会同时标记 V24 与 V25；二者都在 `CSR_RVMin_V` 列表里，
所以两半分别保存/恢复。

代价是两条 m1 16 字节 store，而不是一条 m2 32 字节 store。
这是**优化空间**，不是正确性缺陷；本章不把 `V2M2/V4M2/...` 加进 CSR 列表。

---

## 验证

```sh
ninja -C /home/ubuntu/llvm/builds/pr-ctpop llc llvm-mc llvm-objdump clang
llvm-lit -s llvm/test/CodeGen/RVMin llvm/test/MC/RVMin
llvm/lib/Target/RVMin/scripts/check-rvmin-encodings.sh /home/ubuntu/llvm/builds/pr-ctpop/bin
llvm/lib/Target/RVMin/scripts/check-rvmin-disasm.sh /home/ubuntu/llvm/builds/pr-ctpop/bin
llvm/lib/Target/RVMin/scripts/rvmin-verify-all.sh /home/ubuntu/llvm/builds/pr-ctpop/bin
llvm/lib/Target/RVMin/scripts/rvmin-clang-matrix.sh /home/ubuntu/llvm/builds/pr-ctpop/bin
```

本章实测：

```text
lit: 42/42
V encodings: 41 instructions
V disassembly: 41 instructions
verify-all: 8/8
llc: 276 runs
clang matrix: 432 objects
e2e: 288 objects
```

计数：

| 项 | 第 54 章 | 第 55 章 |
| --- | --- | --- |
| lit（CodeGen+MC/RVMin） | 40 | **42** |
| CodeGen 文件 / not llc | 27 / 6 | **29 / 6** |
| `llc` 门禁用例 | 240 | **276** |
| V 编码 / 反汇编语料 | 41 / 41 | 41 / 41 |
| clang 矩阵 / e2e | 432 / 288 | 432 / 288 |

`v-ext-abi-arg.ll` 与 `v-ext-abi-ret.ll` 从 `RUN: not llc` 翻成正例，
新增 `v-abi.ll`；`v-abi-m2.ll` 作为负例仍带 `RUN: not llc`，
所以 `(29 - 6) × 4 × 3 = 276`。

---

## 验收表

| 编号 | 条件 | 证据 |
| --- | --- | --- |
| AC55-1 | `<4 x i32>` 参数进入 v8 | `v-ext-abi-arg.ll` 的 `vse32.v v8` |
| AC55-2 | 17 个参数时第 17 个从栈加载 | `arg17` 的 `vle32.v ... (a0)` + `addi a0, sp, 0` |
| AC55-3 | `<4 x i32>` 返回在 v8 | `v-ext-abi-ret.ll`、`v-abi.ll` |
| AC55-4 | `v8i32` 参数/返回仍被拒绝 | `v-abi-m2.ll` |
| AC55-5 | `v4f32` 仍被通用诊断拒绝 | 手工 `llc` 命令 |
| AC55-6 | v1–v7、v24–v31 按需保存/恢复 | `v-abi.ll` 的 `callee_saved` |
| AC55-7 | `+v,+f` 同时保存 FPR 与 VR | `v-abi.ll` 的 `fp_v_csr` |
| AC55-8 | mask+m2 混合不破坏 v0 | `v-ext-mask.ll` |
| AC55-9 | 42 lit、29 文件、6 not llc、276 llc | 门禁输出 |
| AC55-10 | V 语料 41、编码/反汇编/432/288 不回归 | 两个脚本 + clang 矩阵 |
| AC55-11 | m2 值跨调用时 V24/V25 两半都保存 | `v-abi.ll` 的 `m2_csr` |
| AC55-12 | `+v,+f` 的 lit 断言 | `v-abi.ll` 的 `FPV` |
| AC55-13 | 10 条 DSL 契约 OK | `dsl-surface.py --check` |
| AC55-14 | 本章八要素文档 | 本文件 |
| AC55-15 | 两个 README 有第 55 行 | diff |
| AC55-16 | `rvmin-ch55` 已推 origin | `git tag` / `git push` |

---

## 常见坑

1. **向量参数寄存器是 16 个，不是 8 个。** v8–v23 共 16 个；
   第 17 个才进栈。把窗口写成 8 会让所有“第 9 个参数在栈”的断言错位。
2. **v0 不是参数寄存器。** `CCAssignToReg` 从 V8 开始；v0 只作为 mask。
   但寄存器分配器可以在没有活跃 mask 的调用点把 v0 当临时（`call17` 就是例子），
   这不是 ABI 分配。
3. **`v8i32` 的错误信息是 LMUL=2 专用。** 它需要偶数对齐的寄存器对，
   不能复用 `CCAssignToReg`；`v4f32` 则是通用“vector arguments”诊断。
4. **栈参数必须 16 字节对齐。** `CCAssignToStack<16, 16>` 和调用点的
   `Align(16)` 要一致，否则 callee 的 `vle32.v` 会读到错误偏移。
5. **VR 没有 DWARF 编号。** 不跳过 CFI 会生成
   `.cfi_offset 4294967295`；这个数字不是合法的 unwind 规则。
   向量 spill 仍然发生，只是没有 CFI 条目。
6. **m2 别名不要凭直觉加。** 当前 alias-aware PEI 已经保存 V24/V25 两半；
   加 `V2M2/...` 之前先看 `m2_csr` 的实测输出。若未来 RA 只保存一半，
   再按上游 `RISCVCallingConv.td:29-40` 加别名，并给 FrameLowering 增加
   `VRM2RegClass` 分支，同时把 CFI 跳过条件扩到 `VRM2RegClass`。
7. **没有 V 时不能 fatal。** `CanLowerReturn` 对向量返回 `return false`，
   让 sret demotion 接管；门禁 6 会用 `-mattr=""`、`+m`、`+c` 跑正例。
8. **改 `.td` 后先 `--update` 再 `--check`。** 第 54 章的行号漂移曾让
   `docs/29/31/32/34/36/37/38` 七个生成块失败；第 55 章的 `.td` 改动
   刷新后，10 条 DSL 契约才恢复 OK。

---

## 练习

1. **m2 参数配对**：给 `<8 x i32>` 参数实现“偶数对齐的 V2M2/V4M2/...”，
   让第 17 个 m2 参数进栈；需要新的 `CCAssignToReg` 约束或自定义 CC 逻辑。
2. **一条 m2 store 代替两条 m1 store**：把 `m2_csr` 的两条 16 字节保存
   合并成一条 32 字节 `vse32.v`（LMUL=2），并验证 epilogue 对称。
3. **`v4f32` ABI**：为 `<4 x float>` 增加参数/返回规则；先决定它与
   `v4i32` 共用 v8–v23 还是单独窗口，再补编码/运行测试。
4. **varargs**：让向量参数出现在可变参数里；注意 RVMin 目前对 varargs
   还有单独的拒绝，需要先确认 ABI 语义。
5. **DWARF**：给 V0–V31 定义 DWARF 寄存器编号，删除 CFI 跳过，
   用 `llvm-dwarfdump --debug-frame` 检查 unwind 表。
