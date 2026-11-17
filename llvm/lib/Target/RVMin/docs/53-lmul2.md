# 第 53 章：LMUL=2——寄存器对、`v8i32` 与 `vmv2r.v`

## 目标

第 48–52 章的向量值都是 128 位：`<4 x i32>`、`<4 x i1>`，LMUL=1。
本章把“一个值占两个向量寄存器”这条链打通：

- `VRM2`：由 `v0/v1`、`v8/v9`、…、`v30/v31` 组成的 256 位寄存器对；
- `<8 x i32>` 成为合法类型，load/store/add 直接选成 m2；
- `vsetivli zero, 8, e32, m2, ta, ma`；
- 32 字节 spill/reload；
- `vmv2r.v`：整寄存器对拷贝；
- 所有编码继续与 `llvm-mc -triple=riscv32 -mattr=+v` 逐字节一致。

本章仍然只做 SEW=32、LMUL=1/2 的整数子集：

- 不做 LMUL=4/8；
- 不做 m2 的向量浮点；
- 不做 `v8i32` 的调用约定（第 55 章只先做 LMUL=1 的 `v4i32` ABI）。

---

## 背景：LMUL 不是“指令变宽了”

RVV 的向量指令编码里没有 LMUL 字段。LMUL 来自 `vtype`：

```text
vsetivli rd, uimm, vtypei
                     ^
                     vtypei = SEW | LMUL | tail | mask
```

对 `e32, m2, ta, ma`，`vtypei = 0xD1`；对 `e32, m1, ta, ma` 是 `0xD0`。
同一条 `vadd.vv` 在 m1 和 m2 下编码完全相同：

```text
vadd.vv v8, v9, v10   # [0x57,0x04,0x95,0x00]  (m1 或 m2 都一样)
```

真正变化的是**解释方式**：

- m1：`vd=v8`，`vs2=v9`，`vs1=v10`，每个寄存器 4 个 32 位元素；
- m2：`vd=v8/v9`，`vs2=v10/v11`，`vs1=v12/v13`，每个组 8 个元素。

因此寄存器对必须满足：

1. 起始寄存器号是偶数；
2. 组内两个寄存器连续；
3. 汇编里仍然写起始寄存器（`v8` 就是 `v8/v9`）。

`vmv2r.v` 是例外：它不看 VL/VTYPE，整对搬运；编码把“寄存器个数”放在
`rs1` 字段，`vmv2r.v` 固定为 1：

```text
vmv2r.v v8, v10   # [0x57,0xb4,0xa0,0x9e]
```

---

## 设计

### 1. 寄存器对：`sub_vrm1_0` / `sub_vrm1_1`

`RVMinRegisterInfo.td` 用两个 128 位子寄存器索引描述一对：

```tablegen
def sub_vrm1_0 : SubRegIndex<128>;
def sub_vrm1_1 : SubRegIndex<128, 128>;

class RVMinVM2Reg<bits<5> num, string n, Register lo, Register hi>
    : RegisterWithSubRegs<n, [lo, hi]> {
  field bits<5> Num = num;
  let Namespace = "RVMin";
  let HWEncoding{4-0} = num;
  let SubRegIndices = [sub_vrm1_0, sub_vrm1_1];
  let CoveredBySubRegs = 1;
}

foreach Index = !range(0, 32, 2) in {
  def V#Index#M2 : RVMinVM2Reg<Index, "v"#Index,
      !cast<Register>("V"#Index), !cast<Register>("V"#!add(Index, 1))>;
}
```

这里没有用 `RegisterTuples`：`RegisterWithSubRegs` 让寄存器对本身就是一个
可分配寄存器，同时保留 `V8`/`V9` 作为子寄存器；`CoveredBySubRegs` 告诉
TableGen“这个类的寄存器由子寄存器完整覆盖”。

`VRM2` 的类型是 `v8i32`，`Size = 256`：

```tablegen
def VRM2 : RegisterClass<"RVMin", [v8i32], 128,
  (add (sequence "V%uM2", 8, 31, 2),
       (sequence "V%uM2", 0, 6, 2))> {
  let Size = 256;
  let DecoderMethod = "DecodeVRM2RegisterClass";
}
```

注意第三个参数仍是 128：它是**栈对齐**，不是寄存器宽度。RVMin 的栈保持
16 字节对齐；spill 槽是 32 字节，但不需要 32 字节栈对齐。

分配顺序和 `VR` 一致：从 `v8M2` 开始，`v0M2` 最后。这样普通代码不会一上来
就占用 `v0/v1`，给第 52 章的 mask 留出空间。

### 2. 指令：m1/m2 共用编码，所以用伪指令

如果直接定义两条真实指令：

```tablegen
def VADD_VV : RVMinVVInst<0b000000, 0b000, "vadd.vv">;
def VADD_VV_M2 : RVMinVVInst<0b000000, 0b000, "vadd.vv">;  // 同样编码
```

TableGen 会报：

```text
error: Decoding conflict: VADD_VV / VADD_VV_M2
```

因为 MC 层只看到 32 位编码，无法区分两者。本章的做法是：

- 真实 MC 指令只保留一份（`VADD_VV`、`VLE32_V`、`VSE32_V`）；
- m2 版本是 `RVMinPseudo`，操作数用 `VRM2`；
- `RVMinAsmPrinter` 在输出时把伪指令展开成真实指令，并让 m2 的 `vsetivli`
  先发出来。

```tablegen
def VLE32_V_M2 : RVMinPseudo<(outs VRM2:$vd), (ins GPR:$rs1), "", []> {
  let mayLoad = 1;
}

def VSE32_V_M2 : RVMinPseudo<(outs), (ins VRM2:$vs3, GPR:$rs1), "", []> {
  let mayStore = 1;
}

def VADD_VV_M2 : RVMinPseudo<(outs VRM2:$vd),
                             (ins VRM2:$vs2, VRM2:$vs1), "", []>;

def VMV2R_V_M2 : RVMinPseudo<(outs VRM2:$vd), (ins VRM2:$vs2), "", []>;
```

`mayLoad`/`mayStore` 不是可选项：伪指令如果没有这两个标志，
`-verify-machineinstrs` 会报 `Missing mayLoad flag` / `Missing mayStore flag`。

### 3. 为什么不用 `PseudoInstExpansion`

TableGen 的自动伪指令展开要求操作数寄存器类完全一致。m2 伪指令是
`VRM2`，真实指令是 `VR`，展开器会报：

```text
Unhandled pseudo-expansion argument type!
```

所以本章在 `RVMinAsmPrinter::emitInstruction` 里手工展开：

```cpp
void emitM2Pseudo(const MachineInstr &MI, unsigned RealOpc) {
  MCInst Inst;
  Inst.setOpcode(RealOpc);
  for (const MachineOperand &MO : MI.operands())
    if (MO.isReg())
      Inst.addOperand(MCOperand::createReg(MO.getReg()));
  emitMCInst(Inst);
}
```

`emitMCInst` 会遍历操作数，发现 `VRM2RegClass` 的寄存器就把 VL/VTYPE
切成 `8 / 0xD1`，然后按需发 `vsetivli`。

### 4. `vmv2r.v`：真实指令，但操作数用 `VR`

`vmv2r.v` 的汇编操作数写 `v8`，而 `v8` 在 MC 层是 `VR` 类的寄存器名。
AsmMatcher 无法用 `VRM2` 去匹配 `v8`（会报 `invalid operand for instruction`），
所以真实 MC 指令的操作数类保持 `VR`：

```tablegen
class RVMinVMV2RInst : RVMinInst<(outs VR:$vd), (ins VR:$vs2),
                                 "vmv2r.v $vd, $vs2", []> {
  let Inst{14-12} = 0b011;
  let Inst{19-15} = 0b00001;   // vmv2r.v 的 rs1 = 1
  let Inst{24-20} = vs2;
  let Inst{25}    = 1;
  let Inst{31-26} = 0b100111;
}
```

偶数约束放在 parser：

```cpp
if (Inst.getOpcode() == RVMin::VMV2R_V) {
  for (unsigned I = 0; I != 2; ++I)
    if (Inst.getOperand(I).getReg() % 2 != 0)
      return Error(IDLoc, "vmv2r.v requires an even register (LMUL=2)");
}
```

codegen 侧仍然用 `VMV2R_V_M2`（`VRM2` 操作数），由 AsmPrinter 展开成
`VMV2R_V`；寄存器分配器看到的是 256 位值。

### 5. CodeGen：只让 `v8i32` 的 add/load/store 合法

`RVMinISelLowering.cpp`：

```cpp
if (STI.hasStdExtV()) {
  addRegisterClass(MVT::v8i32, &RVMin::VRM2RegClass);
}

for (unsigned Op = 0; Op < ISD::BUILTIN_OP_END; ++Op)
  setOperationAction(Op, MVT::v8i32, Expand);
setOperationAction(ISD::ADD, MVT::v8i32, Legal);
setOperationAction(ISD::LOAD, MVT::v8i32, Legal);
setOperationAction(ISD::STORE, MVT::v8i32, Legal);
```

其他操作（`SUB`、`MUL`、`AND`、`VSELECT`、`BUILD_VECTOR`……）先 Expand。
这不是“永远不支持”，而是本章只承诺这一条最小链；后面扩 m2 时逐个打开。

`RVMinISelDAGToDAG.cpp` 按类型选指令：

```cpp
if (VT == MVT::v4i32)
  Opc = RVMin::VLE32_V;
else if (VT == MVT::v8i32)
  Opc = RVMin::VLE32_V_M2;
```

`RVMinInstrInfo.cpp` 增加两条：

- `copyPhysReg`：`VRM2 -> VRM2` 用 `VMV2R_V_M2`；
- spill/reload：`VRM2` 用 `VSE32_V_M2` / `VLE32_V_M2`，槽 32 字节。

---

## 代码地图

| 文件 | 作用 |
| --- | --- |
| `RVMinRegisterInfo.td:112-161` | 子寄存器索引、`RVMinVM2Reg`、`VRM2` |
| `RVMinInstrInfoV.td:270-300,389-405,534-540,553-556` | `vmv2r.v`、m2 伪指令、m2 pattern |
| `RVMinAsmPrinter.cpp:83-96,114-156,172-197` | vset 参数化与缓存、m2 伪指令展开 |
| `RVMinISelDAGToDAG.cpp:260-296` | v4i32/v8i32 的 load/store 选择 |
| `RVMinISelLowering.cpp:136-170` | `v8i32` 寄存器类与合法化 |
| `RVMinInstrInfo.cpp:78-84,300-340` | `copyPhysReg`、spill/reload |
| `RVMinAsmParser.cpp:760-766` | `vmv2r.v` 偶数校验 |
| `Disassembler/RVMinDisassembler.cpp:138-150` | `VRM2` 解码表 |
| `test/MC/RVMin/v-extension.s` | 36 条编码 + `vmv2r.v` 负例 |
| `test/CodeGen/RVMin/v-lmul2.ll` | m2 add、spill、m1/m2 混用、循环 |

---

## 关键实现

### 1. `v8i32` 的 add

```llvm
define void @add8(ptr %a, ptr %b, ptr %out) {
  %va = load <8 x i32>, ptr %a, align 32
  %vb = load <8 x i32>, ptr %b, align 32
  %r = add <8 x i32> %va, %vb
  store <8 x i32> %r, ptr %out, align 32
  ret void
}
```

`-O2` 输出：

```asm
vsetivli zero, 8, e32, m2, ta, ma
vle32.v v8, (a1)
vle32.v v10, (a0)
vadd.vv v8, v10, v8
vse32.v v8, (a2)
```

`vadd.vv` 的 `v8` 实际是 `v8/v9`，`v10` 是 `v10/v11`。

### 2. spill/reload

20 个 volatile `v8i32` 同时活跃时，16 个寄存器对不够，寄存器分配器会溢出：

```asm
addi sp, sp, -160
...
vse32.v v8, (sp)
...
vle32.v v10, (sp)
...
addi sp, sp, 160
```

槽是 32 字节，但栈指针仍按 16 字节对齐。

### 3. m1/m2 混用

同一个块里先做 `<4 x i32>` 再做 `<8 x i32>`，AsmPrinter 会发两次配置：

```asm
vsetivli zero, 4, e32, m1, ta, ma
...
vsetivli zero, 8, e32, m2, ta, ma
...
```

缓存的是 `(VL, VTYPE)` 两个值，不是“有没有发过 vset”一个布尔量。
每个基本块入口 `emitBasicBlockStart` 都会把缓存清零，所以循环 header 和 body
各自带自己的 `vsetivli`。

### 4. `vmv2r.v` 的正例与负例

```asm
vsetivli zero, 8, e32, m2, ta, ma
# [0x57,0x70,0x14,0xcd]
vmv2r.v v8, v10
# [0x57,0xb4,0xa0,0x9e]

vmv2r.v v8, v11
# error: vmv2r.v requires an even register (LMUL=2)
```

反汇编：

```text
$ printf '0x57 0xb4 0xa0 0x9e\n' | llvm-mc -triple=rvmin -mattr=+v -disassemble
vmv2r.v v8, v10
```

---

## 验证

```sh
ninja -C /home/ubuntu/llvm/builds/pr-ctpop llc clang llvm-mc llvm-lit llvm-objdump
llvm-lit -s llvm/test/CodeGen/RVMin llvm/test/MC/RVMin
llvm/lib/Target/RVMin/scripts/check-rvmin-encodings.sh /home/ubuntu/llvm/builds/pr-ctpop/bin
llvm/lib/Target/RVMin/scripts/check-rvmin-disasm.sh /home/ubuntu/llvm/builds/pr-ctpop/bin
llvm/lib/Target/RVMin/scripts/rvmin-verify-all.sh /home/ubuntu/llvm/builds/pr-ctpop/bin
llvm/lib/Target/RVMin/scripts/rvmin-clang-matrix.sh /home/ubuntu/llvm/builds/pr-ctpop/bin
```

本节实测结果：

```text
lit: 38/38
V encodings: 36 instructions
V disassembly: 36 instructions
verify-all: 8/8
llc: 228 runs
clang matrix: 432 objects
e2e: 288 objects
```

计数：

| 项 | 第 52 章 | 第 53 章 |
| --- | --- | --- |
| lit（CodeGen+MC/RVMin） | 37 | **38** |
| V 编码语料 | 34 | **36** |
| 反汇编 V 往返 | 34 | **36** |
| `llc` 门禁用例 | 216 | **228** |
| clang 矩阵 / e2e | 432 / 288 | 不变 |

> 验证过程中发现 `rvmin-clang-matrix.sh` 的 `readelf | grep -q` 在
> `set -o pipefail` 下会偶发 SIGPIPE，误报 “is not EM_RISCV”。
> 脚本已改为先把 ELF header 写到临时文件再 `grep`，432 对象稳定通过。

---

## 验收表

| 编号 | 条件 | 证据 |
| --- | --- | --- |
| AC53-1 | `v8i32` load/store/add 生成 `vsetivli e32,m2` + m2 指令 | `v-lmul2.ll` |
| AC53-2 | 20 个 `v8i32` 值能 spill/reload，32 字节槽 | `v-lmul2.ll` + `-verify-machineinstrs` |
| AC53-3 | `vmv2r.v` 编码与 `riscv32 +v` 一致 | `v-extension.s`、语料 36 |
| AC53-4 | `vmv2r.v` 奇数 vd/vs2 报错 | `v-extension.s` 负例 |
| AC53-5 | m1/m2 同块混用发两次 `vsetivli` | `v-lmul2.ll` |
| AC53-6 | 循环 header/body 各自有正确配置 | `v-lmul2.ll` |
| AC53-7 | llc 228、lit 38、8/8、432、288 | 门禁输出 |
| AC53-8 | DSL 契约 7 后端 OK | `dsl-surface.py --check` |

---

## 常见坑

1. **m1/m2 不能定义成两条真实指令**：编码相同，TableGen 报
   `Decoding conflict`；必须一条真实指令 + 一条伪指令。
2. **`PseudoInstExpansion` 不支持跨寄存器类**：`VRM2 -> VR` 会触发
   `Unhandled pseudo-expansion argument type!`；用 AsmPrinter 手工展开。
3. **伪指令必须带 `mayLoad`/`mayStore`**：否则 `-verify-machineinstrs`
   报 Missing flag。
4. **`vmv2r.v` 的汇编操作数必须是 `VR`**：`v8` 是寄存器对的名字，
   AsmMatcher 不能拿 `VRM2` 去匹配；偶数约束放 parser。
5. **`vmv2r.v` 的 `rs1` 必须是 1**：它编码“搬几个寄存器”，写成 0 会得到
   一条不同的（错误的）编码。
6. **`v8i32` 的测试用 `align 32`**：`align 16` 仍可能生成 m2，但会插入
   标量拷贝/重对齐；不要把两种形状当成等价。
7. **缓存要同时比较 VL 和 VTYPE**：只缓存一个布尔量会在 m1/m2 混用时漏发
   `vsetivli`。
8. **`VRM2` 的 `Size=256`，但 `Alignment` 参数仍是 128**：前者是寄存器宽度，
   后者是栈对齐；写成 256 会让栈要求 32 字节对齐。
9. **`v8i32` 跨调用还没有 ABI**：第 55 章先做 `v4i32`；`v8i32` 参数会明确
   报错，不要误以为已经支持。

---

## 练习

1. 把 `vsub.vv` 扩到 m2：加伪指令、AsmPrinter case、pattern、测试。
2. 实现 LMUL=4（`v16i32`、`vmv4r.v`）：寄存器对变成四元组，
   子寄存器索引和分配顺序要怎么改？
3. 给反汇编器加 `vmv2r.v` 偶数校验：奇数编码应该报错还是按位模式解码？
   两种选择的取舍是什么？
4. 设计 `v8i32` 的调用约定：参数寄存器对必须偶数对齐，
   `CCAssignToReg` 为什么不够？需要怎样的自定义 CC？
