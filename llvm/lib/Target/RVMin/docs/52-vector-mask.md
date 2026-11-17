# 第 52 章：向量 mask——`vm` 位、v0 与 `vmerge.vvm`

## 目标

让 RVMin 的向量子集第一次拥有“掩码”：

- `vm`（bit 25）从固定 1 变成可配置；
- `v0` 成为架构 mask 寄存器，装配层面有两种拼写；
- 新增 `vmerge.vvm` 和 6 条 masked 整数 ALU（`.vv`）；
- `<4 x i1>` 成为合法类型，`select <4 x i1>` 选成 `vmv.v.x v0` + `vmerge.vvm`；
- 所有编码与 `llvm-mc -triple=riscv32 -mattr=+v` 逐字节一致。

本章仍然只做 SEW=32、LMUL=1 的 elementwise mask：不涉及 mask 逻辑指令，
也不支持由 `icmp` 产生的非常量 mask（那是 SETCC 章的内容）。

---

## 背景：mask 为什么不是一个普通值

RVV 的每条向量 ALU 指令都有一个 `vm` 位：

```text
OP-V: funct6 | vm | vs2 | vs1 | funct3 | vd | 1010111
              ^
              vm = 1：不掩码
              vm = 0：使用 v0 作为 mask
```

当 `vm = 0` 时，mask **不占指令里的寄存器号**：它隐含地来自 `v0`。
元素 `i` 的掩码位是 `v0` 第 `i` 个元素的 bit 0。于是：

- masked ALU 的汇编拼写是 `v0.t`（带 `.t`，提醒“这是 mask 不是数据”）；
- `vmerge.vvm` 的 `v0` 是显式操作数，但同样只能是 `v0`，所以拼写不带 `.t`；
- 两种拼写解析后都是同一个物理寄存器 `V0`，编码器把它折成 `vm` 位。

上游 RISC-V 汇编器还禁止 masked 指令写 `v0`：

```text
vadd.vv v0, v31, v1, v0.t
        ^ error: the destination vector register group cannot overlap the mask register
vmerge.vvm v0, v1, v2, v0
           ^ error: the destination vector register group cannot be V0
```

RVMin 在第 52 章把这条约束也实现了，避免教学后端接受架构上非法的编码。

---

## 设计

### 1. 寄存器：`v4i1` 与 `VMV0`

`RVMinRegisterInfo.td`：

- `VR` 的类型列表从 `[v4i32]` 扩成 `[v4i32, v4i1]`；
  mask 与向量共用 128 位寄存器堆，`v4i1` 只用低 4 位；
- 新增 `VMV0`：只含 `V0` 的寄存器类，用来约束 `vmerge` 的 mask 操作数。

`VR` 的分配顺序本来就是 v8→v31→v0→v7，v0 在最后，
所以普通向量值很少占用 mask 寄存器；真正需要 v0 时由寄存器分配器插入拷贝。

### 2. 操作数类：两种拼写，一个寄存器

`RVMinInstrInfoV.td`：

```tablegen
class RVMinVMaskAsmOperand<string name> : AsmOperandClass {
  let Name = name;
  let RenderMethod = "addRegOperands";
  let PredicateMethod = "isV0Reg";
  let DiagnosticType = "InvalidVMaskRegister";
  let DiagnosticString = "operand must be v0.t";
}

def VMaskAsmOperand : RVMinVMaskAsmOperand<"RVMinVMask"> {
  let ParserMethod = "parseV0MaskReg";
  let PredicateMethod = "isV0TReg";
}

def VMergeMaskAsmOperand : RVMinVMaskAsmOperand<"RVMinVMergeMask"> {
  let DiagnosticType = "InvalidVMergeMaskRegister";
  let DiagnosticString = "operand must be v0";
}
```

两个 `DiagnosticType` 必须不同，否则 `RVMinGenAsmMatcher.inc` 会生成重复 case。

`RegisterOperand` 把“打印、编码、解码”三件事绑在一起：

```tablegen
def VMaskOp : RegisterOperand<VMV0> {
  let ParserMatchClass = VMaskAsmOperand;
  let PrintMethod = "printV0MaskReg";
  let EncoderMethod = "getV0MaskReg";
  let DecoderMethod = "DecodeV0MaskReg";
}
```

`VMergeMaskOp` 只改 `ParserMatchClass`，打印仍用默认寄存器名（`v0`）。

### 3. 指令：masked ALU 与 vmerge

- `RVMinVVInstMasked`：`vadd/vsub/vmul/vand/vor/vxor` 的 `.vv` masked 形式，
  操作数是 `vd, vs2, vs1, vm`，`vm` 来自 `VMaskOp`；
- `RVMinVMergeInst`：`vmerge.vvm vd, vs2, vs1, v0`，funct6=`010111`，funct3=`000`；
- **不实现 masked `vmv.v.v`**：上游汇编器不接受 `vmv.v.v vd, vs1, v0.t`
  （`unexpected extra operand`）。需要“掩码移动”时用
  `vmerge.vvm vd, vd, vs1, v0`：mask=0 的元素保留旧 `vd`。

### 4. CodeGen：常量 mask 的物化

`RVMinISelLowering.cpp`：

1. `addRegisterClass(MVT::v4i1, &RVMin::VRRegClass)`，让类型合法化不把
   `<4 x i1>` 拆成标量；
2. `setOperationAction(ISD::VSELECT, MVT::v4i32, Legal)`；
3. `setOperationAction(ISD::BUILD_VECTOR, MVT::v4i1, Custom)`；
4. `lowerMaskBUILD_VECTOR`：把 4 个常量位拼成 0..15 的整数，
   用 `VMV_V_X` 广播到向量寄存器（随后拷进 v0）。

`<4 x i1> <true, false, true, false>` → 整数 `0b0101` = 5 →
`addi a0, zero, 5; vmv.v.x v0, a0`。

### 5. pattern 的操作数顺序

这是本章最容易写错的地方：

```tablegen
def : Pat<(vselect (v4i1 VR:$vm), (v4i32 VR:$vs1), (v4i32 VR:$vs2)),
          (VMERGE_VVM (v4i32 VR:$vs2), (v4i32 VR:$vs1),
                      (COPY_TO_REGCLASS (v4i1 VR:$vm), VMV0))>;
```

- `vselect` 的操作数是 `(cond, true, false)`；
- `vmerge.vvm` 的语义是 `vd = mask ? vs1 : vs2`；
- 所以 **true 必须绑到 `$vs1`，false 绑到 `$vs2`**。

写反不会报错，只会静默生成反值。第 52 章的测试用 MIR 把顺序钉死：
`%a` 来自 `%pa`、`%b` 来自 `%pb`，要求 `VMERGE_VVM %b, %a, %mask`。

---

## 代码地图

| 位置 | 内容 |
| --- | --- |
| `RVMinRegisterInfo.td:117-131` | `VR` 加 `v4i1`；`VMV0` 只含 v0 |
| `RVMinInstrInfoV.td:80-124` | mask 操作数类、`VMaskOp`/`VMergeMaskOp` |
| `RVMinInstrInfoV.td:308-352` | `RVMinVVInstMasked`、`RVMinVMergeInst` |
| `RVMinInstrInfoV.td:380-386` | 6 条 masked ALU |
| `RVMinInstrInfoV.td:494` | `VMERGE_VVM` |
| `RVMinInstrInfoV.td:513-515` | `vselect` pattern |
| `RVMinAsmParser.cpp:455` | `parseV0MaskReg`（识别 `v0.t`） |
| `RVMinAsmParser.cpp:325` | `createMaskReg`（打 `IsMaskToken`） |
| `RVMinAsmParser.cpp:182` | `isV0TReg`（区分 `v0` 与 `v0.t`） |
| `RVMinAsmParser.cpp:750-765` | `vd` 不得为 v0 的校验 |
| `RVMinInstPrinter.cpp:117` | `printV0MaskReg`（打印 `v0.t`） |
| `RVMinMCCodeEmitter.cpp:164` | `getV0MaskReg`（V0→0，NoRegister→1） |
| `RVMinDisassembler.cpp:126` | `DecodeV0MaskReg`（0→V0，1→NoRegister） |
| `RVMinISelLowering.cpp:138-164` | `v4i1` 合法化与动作 |
| `RVMinISelLowering.cpp:491-506` | `lowerMaskBUILD_VECTOR` |
| `test/MC/RVMin/v-extension.s` | 9 条 mask 正例 + 6 条负例 |
| `test/CodeGen/RVMin/v-ext-mask.ll` | MIR 顺序证明 + 非常量 mask 拒绝 |

---

## 关键实现

### 1. `v0.t` 是一个 identifier token

`v0` 和 `.t` 之间没有空白，`parseRegister` 只匹配到 `v0`，剩下 `.t` 会变成
“unexpected extra operand”。所以 `parseV0MaskReg` 先用普通寄存器解析，
再 `Name.consume_back(".t")`，并给操作数打上 `IsMaskToken`。
`isV0Reg` 只看寄存器号，`isV0TReg` 额外要求这个标记——这就是
“`vadd.vv ..., v0`（缺 `.t`）必须报错”的实现方式。

### 2. `vm` 位的编码与解码

- 编码：`getV0MaskReg` 把 `V0` 映射成 0（masked），
  `NoRegister` 映射成 1（unmasked，预留给将来的可选 mask 操作数）；
- 解码：`DecodeV0MaskReg` 把 0 解回 `V0`，1 解回 `NoRegister`；
- 对 `vmerge.vvm`，`vm` 必须为 0；对 masked ALU，`v0.t` 同样为 0。

### 3. 非常量 mask 的处理

`icmp` 产生的 `<4 x i1>` 不是 `BUILD_VECTOR`，会走 `SETCC`。
本章没有 `vmseq.vv/vmslt.vv`，所以 `lowerMaskBUILD_VECTOR` 只接受常量，
否则：

```text
LLVM ERROR: RVMin: non-constant vector masks are not supported yet
```

测试用 `RUN: not llc` 把它固定成“明确拒绝”，而不是静默错码。

---

## 验证

### 1. MC 正例与负例

`llvm/test/MC/RVMin/v-extension.s`：

```text
vmerge.vvm v8, v9, v10, v0      # [0x57,0x04,0x95,0x5c]
vadd.vv v8, v9, v10, v0.t       # [0x57,0x04,0x95,0x00]
vsub.vv ...                     # [0x57,0x04,0x95,0x08]
vmul.vv ...                     # [0x57,0x24,0x95,0x94]
vand.vv ...                     # [0x57,0x04,0x95,0x24]
vor.vv  ...                     # [0x57,0x04,0x95,0x28]
vxor.vv ...                     # [0x57,0x04,0x95,0x2c]
```

负例：

- `vadd.vv ..., v1.t` / 缺 `.t` → `operand must be v0.t`；
- `vmerge.vvm ..., v1` / `v0.t` → `operand must be v0`；
- `vadd.vv v0, ...` / `vmerge.vvm v0, ...` → 目标不得与 mask 重叠。

### 2. CodeGen：MIR 证明顺序

`llvm/test/CodeGen/RVMin/v-ext-mask.ll`：

```text
%3:vr = VLE32_V %0 :: (load (s128) from %ir.pa)
%4:vr = VLE32_V %1 :: (load (s128) from %ir.pb)
%5:gpr = ADDI $x0, 5
%6:vr = VMV_V_X %5
%7:vmv0 = COPY %6
%8:vr = VMERGE_VVM %4, %3, %7
```

`%4` 是 false（`%pb`），`%3` 是 true（`%pa`），顺序正确。
测试同时要求 `llc` 的输出能被 `riscv32 +v` 重新汇编。

### 3. 计数

| 项 | 第 51 章 | 第 52 章 |
| --- | --- | --- |
| lit（CodeGen+MC/RVMin） | 36 | **37** |
| V 编码语料 | 27 | **34** |
| 反汇编 V 往返 | 27 | **34** |
| `llc` 门禁用例 | 216 | **216**（新测试含 1 条 `RUN: not llc`，与文件增量抵消） |
| clang 矩阵 / e2e | 432 / 288 | 不变 |

---

## 验收表

| 编号 | 条件 | 证据 |
| --- | --- | --- |
| AC52-1 | 7 条 mask 指令编码与 `riscv32 +v` 逐字节一致 | `check-rvmin-encodings.sh` |
| AC52-2 | 反汇编往返一致 | `check-rvmin-disasm.sh` |
| AC52-3 | 4 类 mask 拼写错误 + 2 类 vd=v0 错误 | `v-extension.s` |
| AC52-4 | `select <4 x i1>` 常量 mask 生成 `vmv.v.x v0` + `vmerge.vvm` | `v-ext-mask.ll` |
| AC52-5 | MIR 中 true 值在 `vs1` | `v-ext-mask.ll` |
| AC52-6 | 非常量 mask 明确拒绝 | `RUN: not llc` |
| AC52-7 | lit 37/37、8/8、llc 216、clang 432、e2e 288 | 门禁输出 |
| AC52-8 | DSL 契约 OK | `dsl-surface.py --check` |

---

## 常见坑

1. **两个 mask 操作数类的 `DiagnosticType` 不能相同**：会生成重复 case，编译失败。
2. **`DecoderMethod` 是 C++ 函数名**：写 `decodeV0MaskReg` 会找不到符号，
   必须与实现同名（`DecodeV0MaskReg`）。
3. **`v0.t` 不是两个 token**：必须专门解析并保留 `IsMaskToken`，
   否则 `v0` 和 `v0.t` 无法区分。
4. **masked ALU 与 vmerge 的拼写相反**：前者 `v0.t`，后者 `v0`；
   写错时诊断分别是 `operand must be v0.t` / `operand must be v0`。
5. **`vselect` 的 true 必须绑到 `vs1`**：写反不报错，只会静默取反。
6. **`v4i1` 的 `BUILD_VECTOR` 不能 Expand**：会走
   `ReplaceNodeResults not implemented`；必须注册寄存器类并设成 Custom。
7. **masked 指令的 `vd` 不能是 v0**：架构上非法，装配器要拒绝。

---

## 练习

1. 给 `vmerge.vxm`/`vmerge.vim` 加 MC 支持，并对照 `riscv32 +v` 编码。
2. 实现 `vmseq.vv`，让 `icmp eq <4 x i32>` 直接产生 mask，
   把 `v-ext-mask.ll` 的非常量负例改成正例。
3. 观察 `VR` 分配顺序：把 `v0` 从 `VR` 中移除（只留 `VMV0`），
   哪些测试会失败？为什么 `VMV0` 单独存在还不够？
