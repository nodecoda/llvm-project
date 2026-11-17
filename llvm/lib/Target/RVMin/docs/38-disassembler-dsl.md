# 第 38 章：第七个后端——`-gen-disassembler` 与目标自定义 encoding

## 目标

前六个后端都在回答同一个问题：**这个后端从通用 schema 里读哪些字段？**
第 38 章的反汇编器给出一个不同的答案：

> 它最重要的输入 `Inst` 根本不在 `Target.td` 的通用 schema 里，而是每个
> target 自己声明的 `field bits<32> Inst`。

所以本章要验证的是契约的**边界诚实性**：

- 通用字段仍然可以按 `(declaring class, field)` 描述；
- 目标自定义字段必须被明确标成“自定义”，而不是假装它不存在；
- 一个 `-gen-*` 选项不一定对应一个 emitter——`-gen-disassembler` 是分发器。

七后端总表：

| 后端 | roots | 字段 | 消费 | derive / project / both | schema | magic | derivations |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `register-info` | 8 | 59 | 44 | 39 / 4 / 1 | 15 | 10 | 11 |
| `instr-info` | 6 | 111 | 85 | 74 / 5 / 6 | 26 | 0 | 8 |
| `asm-writer` | 8 | 130 | 99 | 84 / 9 / 6 | 31 | 2 | 8 |
| `asm-matcher` | 15 | 166 | 142 | 95 / 42 / 5 | 24 | 2 | 7 |
| `dag-isel` | 14 | 186 | 136 | 135 / 1 / 0 | 50 | 6 | 7 |
| `fast-isel` | 14 | 186 | 136 | 133 / 1 / 2 | 50 | 0 | 4 |
| `disassembler` | **8** | **127** | **96** | **88 / 4 / 4** | **31** | **0** | **4** |

本章产出：

- `dsl-surface.py` 增加 `--backend disassembler`；
- 契约：8 个记录类入口、127 个字段、96 个被消费、52 行投影矩阵、
  4 条派生改写、0 个硬编码名字；
- 4 个纯 `project` 字段：`DecoderNamespace` / `EncodingInfos` /
  `isAsmParserOnly` / `AliasOf`；
- 4 个 `derive+project` 字段：`Size` / `Predicates` / `isCodeGenOnly` /
  `AssemblerMatcherPredicate`；
- `--usage` 第一次把核心输入 `Inst` 的声明行标成“自定义字段”，
  而 `let Inst{...}` 赋值行显示“不在本后端契约”。

非目标：

- 不覆盖 X86 / WebAssembly 的自定义反汇编路径。同一个
  `-gen-disassembler` 选项在它们身上会路由到完全不同的 source set；
- 不修改 `DecoderEmitter` / `DecoderTree` 的行为；
- 不实现新的解码算法；本章只分析 TableGen 投影。

## 背景：为什么反汇编器是第七个

前六个后端的投影方向都是“record → 生成物”：

```text
Instruction / Register / Predicate → 表或匹配器
```

反汇编是逆过程：

```text
encoding bits → 匹配树 → MCInst
```

`DisassemblerEmitter.cpp` 只有 133 行，因为它是一个分发器：

```text
-gen-disassembler
   ├─ X86          → X86DisassemblerTables + RecognizableInstr
   ├─ WebAssembly  → WebAssemblyDisassemblerEmitter
   └─ 其它         → EmitDecoder (DecoderEmitter.cpp)
```

RVMin 走最后一条：

```text
RVMinInstrFormats.td:  field bits<32> Inst;
        │
        ▼
DecoderEmitter: 收集 Instruction / AdditionalEncoding / EncodingInfos
        │
        ▼
DecoderTree:    按 bit pattern 建解码树
        │
        ▼
GenDisassemblerTables.inc
```

关键点：`Inst` 不是在 `Target.td` 里声明的，而是在每个 target 的
`InstrFormats.td` 里用 `field bits<32> Inst` 自己加的。通用契约的 127 个
字段里没有它；`--usage` 会把它的 `field` 声明标成“自定义字段”。

## 代码地图

| 文件 | 作用 |
| --- | --- |
| `llvm/utils/TableGen/DisassemblerEmitter.cpp` | 选项入口与三分支分发 |
| `llvm/utils/TableGen/DecoderEmitter.cpp` | 通用定长/变长解码器 emitter，约 1600 行 |
| `llvm/utils/TableGen/DecoderTableEmitter.{h,cpp}` | 解码表的分层与压缩 |
| `llvm/utils/TableGen/DecoderTree.{h,cpp}` | 位模式过滤树 |
| `llvm/utils/TableGen/Common/CodeGenInstruction.{h,cpp}` | 指令/操作数模型、`DecoderMethod` 缓存 |
| `llvm/utils/TableGen/Common/CodeGenRegisters.{h,cpp}` | 寄存器类与 `RegClassByHwMode` |
| `llvm/utils/TableGen/Common/CodeGenTarget.{h,cpp}` | 指令枚举与非伪指令列表 |
| `llvm/utils/TableGen/Common/CodeGenHwModes.{h,cpp}` | `HwModeSelect` 的 `Modes` / `Objects` |
| `llvm/utils/TableGen/Common/InfoByHwMode.{h,cpp}` | `EncodingInfoByHwMode` |
| `llvm/utils/TableGen/Common/SubtargetFeatureInfo.{h,cpp}` | `Predicate` → feature 检查 |
| `llvm/include/llvm/Target/Target.td` | `InstructionEncoding` / `Instruction` / `AdditionalEncoding` |
| `llvm/include/llvm/Target/TargetSubtarget.td` | `Predicate` |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td` | **RVMin 的 `field bits<32> Inst`** |
| `llvm/utils/TableGen/dsl-surface.py` | **本章改造**：第七个 Backend + 16 个注解 |
| `llvm/lib/Target/RVMin/docs/38-disassembler-dsl.md` | 本章；四个生成块由脚本生成 |

## 关键实现：第七个后端怎么接进来

### 1. sources：一个选项，只取其中一条路径

```python
"disassembler": Backend(
    name="disassembler",
    emitter="llvm/utils/TableGen/DisassemblerEmitter.cpp",
    sources=(
        ("llvm/utils/TableGen/DisassemblerEmitter.cpp", "emitter"),
        ("llvm/utils/TableGen/DecoderEmitter.cpp", "emitter"),
        ("llvm/utils/TableGen/DecoderTableEmitter.cpp", "emitter"),
        ("llvm/utils/TableGen/DecoderTableEmitter.h", "emitter"),
        ("llvm/utils/TableGen/DecoderTree.cpp", "emitter"),
        ("llvm/utils/TableGen/DecoderTree.h", "emitter"),
        # shared: CodeGenInstruction / CodeGenRegisters / CodeGenTarget /
        #         CodeGenHwModes / InfoByHwMode / SubtargetFeatureInfo
    ),
)
```

没有包含 `X86DisassemblerEmitter` / `X86RecognizableInstr` /
`WebAssemblyDisassemblerEmitter`。这不是漏了，而是**契约范围**：
它描述的是 RVMin 走的 generic path，而不是同一个选项在 X86 上的另一套
实现。第 37 章也做过类似的范围声明（只分析 TableGen 投影，不实现
RVMin FastISel）；本章第一次出现“同一个 `-gen-*` 选项，不同 source set”。

### 2. roots：8 个入口

| 记录类 | 来源 | 为什么是根 |
| --- | --- | --- |
| `Instruction` | `CodeGenTarget.cpp` | 解码器遍历所有非伪指令 |
| `AdditionalEncoding` | `DecoderEmitter.cpp` | 额外 encoding，`AliasOf` 指向真实指令 |
| `RegClassByHwMode` | `CodeGenTarget.cpp` | 按 HwMode 选择寄存器类的解码方法 |
| `Register` / `RegisterClass` | `CodeGenRegisters.cpp` | 操作数寄存器类解析 |
| `Operand` / `RegisterOperand` | `InstrInfoEmitter.cpp` | 操作数的 `DecoderMethod` / `MCOperandPredicate` |
| `Predicate` | `SubtargetFeatureInfo.cpp` | 解码器的 feature 检查 |

`roots_from` 只列 `DecoderEmitter.cpp` 与 `SubtargetFeatureInfo.cpp`，
其余通过 `extra_roots` 显式登记；这样不会把 `CodeGenTarget.cpp` 里的
`FusionPredicates` / `Target` 一并带进 schema。

### 3. helper_classes：`EncodingByHwMode` 与 `HwModeSelect`

`Instruction.EncodingInfos` 的类型是 `EncodingByHwMode`，它继承
`HwModeSelect`：

```tablegen
class HwModeSelect<list<HwMode> Ms, int ObjectsLength> {
  list<HwMode> Modes = Ms;
}

class EncodingByHwMode<list<HwMode> Ms = [], list<InstructionEncoding> Ts = []>
    : HwModeSelect<Ms, !size(Ts)> {
  list<InstructionEncoding> Objects = Ts;
}
```

`Objects` 声明在派生类 `EncodingByHwMode` 上，所以它和 `HwModeSelect`
都必须列进 `helper_classes`；否则 `EncodingInfos` 的 HwMode 语义在契约里
只剩一个字段名。

其余 helper 是 `DAGOperand`（`DecoderMethod` / `hasCompleteDecoder` 的
声明类）、`Predicate`、`ValueType`，以及寄存器/HwMode 模型的一组包装类。

### 4. 注解：从 instr-info / register-info / asm-matcher 组合

反汇编器的 127 个字段大部分与 instr-info 共享，所以注解按来源组合：

- instr-info 的基础注解（排除只属于 instr-info 的 `Body` /
  `FunctionName` / `Instruction`）；
- register-info 的 8 个寄存器字段；
- asm-matcher 的 4 个 `Predicate` 字段；
- 4 个 decoder 专属字段：`DecoderNamespace` / `EncodingInfos` /
  `isAsmParserOnly` / `AliasOf`。

### 5. 派生改写：4 条

- encoding collection：收集非伪指令；
- additional encodings：`AdditionalEncoding` 归并到 `AliasOf`；
- hwmode encoding select：`EncodingInfos` → `EncodingByHwMode.Objects`；
- predicate lowering：`Predicates` → MCDecoder feature 检查。

### 6. 投影特征：88 derive / 4 project / 4 both

纯 `project` 的 4 个字段正好是解码器的直接开关：

| 字段 | 声明于 | 读取点 | 语义 |
| --- | --- | --- | --- |
| `DecoderNamespace` | `InstructionEncoding` | `DecoderEmitter.cpp:1336` | 选择子表 |
| `EncodingInfos` | `Instruction` | `DecoderEmitter.cpp:1434` | 非空时忽略 `Inst` |
| `isAsmParserOnly` | `Instruction` | `DecoderEmitter.cpp:1379` | 跳过只给汇编器的指令 |
| `AliasOf` | `AdditionalEncoding` | `DecoderEmitter.cpp:1465` | 归并到真实指令 |

`derive+project` 的 4 个字段说明“共享模型 + emitter 都读”：

| 字段 | 共享读取点 | emitter 读取点 |
| --- | --- | --- |
| `Size` | `CodeGenRegisters.cpp` / `InfoByHwMode.cpp` | `DecoderEmitter.cpp:1391` |
| `Predicates` | `CodeGenHwModes.cpp:27` | `DecoderEmitter.cpp:768` |
| `isCodeGenOnly` | `CodeGenInstruction.cpp` | `DecoderEmitter.cpp:1380` |
| `AssemblerMatcherPredicate` | `SubtargetFeatureInfo.cpp` | `DecoderEmitter.cpp:770` |

`derive` 有 88 个，来自 `CodeGenInstruction` / `CodeGenRegisters` /
`CodeGenTarget` 的正常建模；这说明反汇编器的大部分输入仍然是共享层准备好的
指令/寄存器模型，只有最后一层“位模式 → MCInst”由 emitter 决定。

### 7. 契约的边界：`Inst` 是自定义字段

这是本章最重要的观察：

- 通用 schema 有 127 个字段，但没有 `Inst`；
- `Inst` 在 `RVMinInstrFormats.td` 里由 `field bits<32> Inst` 声明；
- `DecoderEmitter` 通过 `EncodingDef->getValue("Inst")` 直接读它；
- `--usage` 会把 `RVMinInstrFormats.td` 里的 `field bits<32> Inst` 声明标成
  “自定义字段”；`let Inst{...} = ...` 这类赋值行则显示“不在本后端契约”，
  因为工具只解析 `Target.td` 的通用 schema，不知道 RVMin 自己的字段声明。

也就是说：**契约能诚实地说“这不是我认识的 schema 字段”，而不是假装
它不存在。** 前七章一直在扩大“能描述的字段”；本章第一次明确画出
“不能描述、但必须点名的字段”。

## 契约与投影

下面四段由脚本生成，不要手改；改 emitter 或 `.td` 后运行 `--update`。

<!-- BEGIN: GENERATED DSL CONTRACT -->

后端：`None` · emitter：`llvm/utils/TableGen/DisassemblerEmitter.cpp`
统计：8 个记录类入口 · 127 个字段（schema 31 · derive 88 · project 4 · derive+project 4）。

**记录类入口**

| 记录类 | 定义 | 消费证据 |
| --- | --- | --- |
| `Register` | `llvm/include/llvm/Target/Target.td:173` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:1264` |
| `RegisterClass` | `llvm/include/llvm/Target/Target.td:292` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:1354` |
| `Instruction` | `llvm/include/llvm/Target/Target.td:608` | `llvm/utils/TableGen/Common/CodeGenTarget.cpp:287` |
| `AdditionalEncoding` | `llvm/include/llvm/Target/Target.td:842` | `llvm/utils/TableGen/DecoderEmitter.cpp:1464` |
| `RegClassByHwMode` | `llvm/include/llvm/Target/Target.td:912` | `llvm/utils/TableGen/Common/CodeGenTarget.cpp:118` |
| `Operand` | `llvm/include/llvm/Target/Target.td:1001` | `llvm/utils/TableGen/InstrInfoEmitter.cpp:456` |
| `RegisterOperand` | `llvm/include/llvm/Target/Target.td:1051` | `llvm/utils/TableGen/InstrInfoEmitter.cpp:458` |
| `Predicate` | `llvm/include/llvm/Target/TargetSubtarget.td:78` | `llvm/utils/TableGen/Common/SubtargetFeatureInfo.cpp:27` |

**字段契约**（主键是 `(declaring class, field)`；`stage` 表示它停在哪一层，`⚠` 表示同名多声明）

| 字段 | 声明于 | 类型 | 默认值 | stage | 本后端读取点 | 语义 |
| --- | --- | --- | --- | --- | --- | --- |
| `Modes` | `HwModeSelect` | `list<HwMode>` | `Ms` | `derive` | `llvm/utils/TableGen/Common/CodeGenHwModes.cpp:40` | HwMode 列表；与 `Objects` 等长，按 ModeId 绑定。 |
| `RegSize` | `RegInfo` | `int` | `RS` | `derive` | `llvm/utils/TableGen/Common/InfoByHwMode.cpp:105` | `RegInfo` 的寄存器位数。 |
| `SpillAlignment` | `RegInfo` | `int` | `SA` | `derive` | `llvm/utils/TableGen/Common/InfoByHwMode.cpp:107` | `RegInfo` 的 spill slot 对齐。 |
| `SpillSize` | `RegInfo` | `int` | `SS` | `derive` | `llvm/utils/TableGen/Common/InfoByHwMode.cpp:106` | `RegInfo` 的 spill slot 位数。 |
| `Objects` | `RegInfoByHwMode`, `SubRegRangeByHwMode`, `EncodingByHwMode`, `RegClassByHwMode` | `list<RegInfo>` / `list<SubRegRange>` / `list<InstructionEncoding>` / `list<RegisterClass>` | `Ts` / `RegClasses` | `derive` ⚠ | `llvm/utils/TableGen/Common/CodeGenHwModes.cpp:41`, `llvm/utils/TableGen/Common/CodeGenTarget.cpp:119` | 按 HwMode 绑定的对象列表；`RegisterByHwMode` 是寄存器，`RegInfoByHwMode`/`SubRegRangeByHwMode` 是参数。 |
| `Offset` | `SubRegRange` | `int` | `offset` | `derive` | `llvm/utils/TableGen/Common/InfoByHwMode.cpp:189` | 子寄存器/范围的起始位；`-1` 表示不固定。 |
| `Size` | `SubRegRange`, `RegisterClass`, `InstructionEncoding`, `Instruction` | `int` | `size` / `0` / `（未设默认值）` | `derive+project` ⚠ | `llvm/utils/TableGen/DecoderEmitter.cpp:1391`, `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:800`, `llvm/utils/TableGen/Common/InfoByHwMode.cpp:188` | 指令编码字节数；`emitRecord` 写入 MCInstrDesc 的 Size。同名 `RegisterClass.Size` 是 spill size。 |
| `Aliases` | `Register` | `list<Register>` | `[]` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:193` | 显式别名；对称化成别名图，参与 regunit / `getSubRegs()` / lane mask 计算。 |
| `AsmName` | `Register` | `string` | `n` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:1307` | 汇编名；用作 `RegistersByName` 去重 key。`RegisterTuples` 未给 `RegAsmNames` 时为空串。 |
| `CostPerUse` | `Register` | `list<int>` | `[0]` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:166`, `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:668` | 每寄存器成本序列；按全局最大长度补 0，写入 `CostPerUseTable`。 |
| `CoveredBySubRegs` | `Register` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:167` | 值完全由子寄存器决定；触发 covered-register 计算与合成子寄存器索引。 |
| `Namespace` | `Register`, `RegisterClass`, `Instruction` | `string` | `""` / `namespace` | `derive` ⚠ | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:419`, `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:58`, `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:59`, `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:796` …(+3) | 指令/寄存器命名空间；`Target.getInstNamespace()` 决定生成 enum 的 C++ namespace。 |
| `SubRegIndices` | `Register` | `list<SubRegIndex>` | `[]` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:175`, `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:632` | 与 `SubRegs` 一一对应；`RegisterTuples` 展开时取自 tuple 定义。 |
| `SubRegs` | `Register` | `list<Register>` | `[]` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:176`, `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:634` | 子寄存器列表；与 `SubRegIndices` 交给 `zip_equal` 配对。`RegisterTuples` 展开时被替换成 tuple。 |
| `isArtificial` | `Register` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:170` | 合成寄存器标记；写入 `MCRegisterDesc`，并参与寄存器类的 `Artificial` 计算（全 artificial 的类压力权重上限为 0）。 |
| `isConstant` | `Register` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:168` | 生成 `isConstantPhysReg()`，并自动加入所有 `CalleeSavedRegs` 的 mask。 |
| `OperandNamespace` | `DAGOperand` | `string` | `"MCOI"` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:89`, `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:94` | MC operand 类型所在 namespace；与 `OperandType` 一起构成完整类型名。 |
| `Alignment` | `RegisterClass` | `int` | `alignment` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:807` | 寄存器类 spill 对齐（bit）；写入 `RegClassInfo::SpillAlignment`。 |
| `AllocationPriority` | `RegisterClass` | `int` | `0` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:818` | 寄存器分配优先级；越高越先分配。 |
| `AltOrderSelect` | `RegisterClass` | `code` | `[{}]` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:813` | `code` 注入点；非空时生成替代分配顺序选择函数。 |
| `AltOrders` | `RegisterClass` | `list<dag>` | `[]` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:764` | 备用分配顺序列表；与 `AltOrderSelect` 配合。 |
| `BaseClassOrder` | `RegisterClass` | `int` | `?` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.h:619` | 寄存器类在继承层次中的顺序；影响类排序。 |
| `CopyCost` | `RegisterClass` | `int` | `1` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:811` | 寄存器拷贝代价；8-bit，>255 fatal，负值实际按 uint8_t 截断。 |
| `GeneratePressureSet` | `RegisterClass` | `bit` | `true` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:758` | 是否生成寄存器压力集。 |
| `GlobalPriority` | `RegisterClass` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:823` | 全局分配优先级。 |
| `MemberList` | `RegisterClass` | `dag` | `regList` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:1249` | 不是普通 list：经 SetTheory field expander 展开，决定寄存器类成员集与默认分配序。 |
| `RegInfos` | `RegisterClass` | `RegInfoByHwMode` | `（未设默认值）` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:798` | 按 HwMode 的 `{RegSize,SpillSize,SpillAlignment}` 三元组；覆盖默认模式值。 |
| `RegTypes` | `RegisterClass` | `list<ValueType>` | `regTypes` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:759` | 寄存器类支持的 MVT 列表；`Size==0` 时用于推断位宽。 |
| `SpillStackID` | `RegisterClass` | `int` | `0` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:814` | spill stack slot 的自定义 ID；影响 frame index 分配。 |
| `TSFlags` | `RegisterClass`, `Instruction` | `bits<8>` / `bits<64>` | `0` | `derive` ⚠ | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:825` | 目标自定义标志位；`emitRecord` 校验位宽并写入 MCInstrDesc 的 TSFlags。同名 `RegisterClass.TSFlags` 是寄存器类标志。 |
| `Weight` | `RegisterClass` | `int` | `?` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:920` | 寄存器类的压力权重；影响压力集计算。 |
| `isAllocatable` | `RegisterClass` | `bit` | `true` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:812` | 是否可分配；决定 `InAllocatableClassTable`。 |
| `DecoderNamespace` | `InstructionEncoding` | `string` | `""` | `project` | `llvm/utils/TableGen/DecoderEmitter.cpp:1336` | 解码器命名空间；`decodeInstruction` 按它选择子表。 |
| `Predicates` | `InstructionEncoding`, `Instruction` | `list<Predicate>` | `[]` | `derive+project` ⚠ | `llvm/utils/TableGen/DecoderEmitter.cpp:768`, `llvm/utils/TableGen/Common/CodeGenHwModes.cpp:27` | 指令谓词列表；`computeRequiredFeatures` 用它编码每个 opcode 的 feature bitset。 |
| `AsmString` | `Instruction` | `string` | `""` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:420` | 汇编模板；`CodeGenInstruction` 保存，供 asm writer/matcher 使用；本后端只解析不直接发射。 |
| `Constraints` | `Instruction` | `string` | `""` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:480` | 内联汇编约束字符串；`ParseConstraints` 为每个操作数填约束位。 |
| `Defs` | `Instruction` | `list<Register>` | `[]` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:472` | 隐式定义寄存器列表；合并进 `ImplicitDefs`，发射到 `ImplicitLists`。 |
| `EncodingInfos` | `Instruction` | `EncodingByHwMode` | `（未设默认值）` | `project` | `llvm/utils/TableGen/DecoderEmitter.cpp:1434` | 按 HwMode 的 encoding 选择；非空时忽略 `Instruction.Inst`。 |
| `FastISelShouldIgnore` | `Instruction` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:453` | FastISel 忽略该指令；只保存在 `CodeGenInstruction`，不进入 MCInstrDesc。 |
| `InOperandList` | `Instruction` | `dag` | `（未设默认值）` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:43` | 输入操作数 dag；展开成 use 列表，参与操作数编号与 `MIOperandInfo` 解析。 |
| `OutOperandList` | `Instruction` | `dag` | `（未设默认值）` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:29` | 输出操作数 dag；`CodeGenInstruction` 展开成 def 列表，决定 `NumDefs`。 |
| `Uses` | `Instruction` | `list<Register>` | `[]` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:473` | 隐式使用寄存器列表；合并进 `ImplicitUses`，发射到 `ImplicitLists`。 |
| `canFoldAsLoad` | `Instruction` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:436` | MCID::FoldableAsLoad。 |
| `hasCtrlDep` | `Instruction` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:446` | MCID::HasCtrlDep。 |
| `hasDelaySlot` | `Instruction` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:443` | MCID::HasDelaySlot。 |
| `hasExtraDefRegAllocReq` | `Instruction` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:468` | MCID::ExtraDefRegAllocReq。 |
| `hasExtraSrcRegAllocReq` | `Instruction` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:467` | MCID::ExtraSrcRegAllocReq。 |
| `hasNoSchedulingInfo` | `Instruction` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:452` | 跳过调度类分配；`SchedModels.getSchedClassIdx` 返回 0。 |
| `hasPostISelHook` | `Instruction` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:445` | MCID::HasPostISelHook。 |
| `hasSideEffects` | `Instruction` | `bit` | `?` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:463` | MCID::MayLoad/MayStore 之外的副作用；值为 `?` 时由 pattern 推断。 |
| `isAdd` | `Instruction` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:434` | MCID::Add。 |
| `isAsCheapAsAMove` | `Instruction` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:466` | MCID::AsCheapAsAMove。 |
| `isAsmParserOnly` | `Instruction` | `bit` | `false` | `project` | `llvm/utils/TableGen/DecoderEmitter.cpp:1379` | 只给汇编器用的指令；反汇编器跳过。 |
| `isAuthenticated` | `Instruction` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:455` | MCID::Authenticated。 |
| `isBarrier` | `Instruction` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:432` | MCID::Barrier；控制流不能 fall through。 |
| `isBitcast` | `Instruction` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:430` | MCID::Bitcast。 |
| `isBranch` | `Instruction` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:425` | MCID::Branch。 |
| `isCall` | `Instruction` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:433` | MCID::Call。 |
| `isCodeGenOnly` | `Instruction` | `bit` | `false` | `derive+project` | `llvm/utils/TableGen/DecoderEmitter.cpp:1380`, `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:469` | 仅 codegen 使用，汇编器不可见；影响 asm matcher 与 MCID 标志。 |
| `isCommutable` | `Instruction` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:440` | MCID::Commutable。 |
| `isCompare` | `Instruction` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:427` | MCID::Compare。 |
| `isConvergent` | `Instruction` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:451` | MCID::Convergent。 |
| `isConvertibleToThreeAddress` | `Instruction` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:439` | MCID::ConvertibleTo3Addr。 |
| `isEHScopeReturn` | `Instruction` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:424` | MCID::EHScopeReturn。 |
| `isExtractSubreg` | `Instruction` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:449` | MCID::ExtractSubreg。 |
| `isIndirectBranch` | `Instruction` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:426` | MCID::IndirectBranch。 |
| `isInsertSubreg` | `Instruction` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:450` | MCID::InsertSubreg。 |
| `isMeta` | `Instruction` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:471` | MCID::Meta；元指令，不参与调度/编码。 |
| `isMoveImm` | `Instruction` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:428` | MCID::MoveImm。 |
| `isMoveReg` | `Instruction` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:429` | MCID::MoveReg。 |
| `isNotDuplicable` | `Instruction` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:447` | MCID::NotDuplicable。 |
| `isPreISelOpcode` | `Instruction` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:422` | MCID::PreISelOpcode；pre-isel 伪操作码。 |
| `isPredicable` | `Instruction` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:438` | MCID::Predicable。 |
| `isPseudo` | `Instruction` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:470`, `llvm/utils/TableGen/Common/CodeGenTarget.cpp:347`, `llvm/utils/TableGen/Common/CodeGenTarget.cpp:388` | MCID::Pseudo；伪指令。 |
| `isReMaterializable` | `Instruction` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:442` | MCID::Rematerializable。 |
| `isRegSequence` | `Instruction` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:448` | MCID::RegSequence。 |
| `isReturn` | `Instruction` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:423` | MCID::Return。 |
| `isSelect` | `Instruction` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:431` | MCID::Select。 |
| `isTerminator` | `Instruction` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:441` | MCID::Terminator。 |
| `isTrap` | `Instruction` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:435` | MCID::Trap。 |
| `isUnpredicable` | `Instruction` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:437` | MCID::Unpredicable。 |
| `mayLoad` | `Instruction` | `bit` | `?` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:458` | MCID::MayLoad；值为 `?` 时由 pattern 推断。 |
| `mayRaiseFPException` | `Instruction` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:462` | MCID::MayRaiseFPException。 |
| `mayStore` | `Instruction` | `bit` | `?` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:460` | MCID::MayStore；值为 `?` 时由 pattern 推断。 |
| `usesCustomInserter` | `Instruction` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:444` | MCID::UsesCustomInserter。 |
| `variadicOpsAreDefs` | `Instruction` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:454` | MCID::VariadicOpsAreDefs。 |
| `AliasOf` | `AdditionalEncoding` | `Instruction` | `I` | `project` | `llvm/utils/TableGen/DecoderEmitter.cpp:1465` | `AdditionalEncoding` 指向的真实指令；解码结果归到它。 |
| `EncoderMethod` | `Operand`, `RegisterOperand` | `string` | `""` | `derive` ⚠ | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:90`, `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:96`, `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:181` | 自定义编码函数名；`CodeGenInstruction` 保存，供 encoder 调用。 |
| `MIOperandInfo` | `Operand` | `dag` | `(ops)` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:97` | 复合操作数的子操作数 dag；`CollectOperandInfo` 用它展开 MC operand 列表。 |
| `OperandType` | `Operand`, `RegisterOperand` | `string` | `"OPERAND_UNKNOWN"` / `"OPERAND_REGISTER"` | `derive` ⚠ | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:88`, `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:93` | MC operand 的 C++ 类型名；`CodeGenInstruction` 存入 `OperandType`。 |
| `PrintMethod` | `Operand`, `RegisterOperand` | `string` | `"printOperand"` / `pm` | `derive` ⚠ | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:87`, `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:92` | 自定义打印函数名；`CodeGenInstruction` 保存，供 asm writer 调用。 |
| `RegClass` | `RegisterOperand` | `RegisterClassLike` | `regclass` | `derive` | `llvm/utils/TableGen/Common/CodeGenTarget.cpp:274`, `llvm/utils/TableGen/Common/InfoByHwMode.cpp:224` | 操作数绑定的寄存器类；解析为 `RegisterClass` 并用于寄存器操作数信息。 |
| `AssemblerCondDag` | `Predicate` | `dag` | `（未设默认值）` | `derive` | `llvm/utils/TableGen/Common/SubtargetFeatureInfo.cpp:181`, `llvm/utils/TableGen/Common/SubtargetFeatureInfo.cpp:203`, `llvm/utils/TableGen/Common/SubtargetFeatureInfo.cpp:224` | 汇编器谓词的 dag 条件；展开成 feature 检查代码。 |
| `AssemblerMatcherPredicate` | `Predicate` | `bit` | `false` | `derive+project` | `llvm/utils/TableGen/DecoderEmitter.cpp:770`, `llvm/utils/TableGen/Common/SubtargetFeatureInfo.cpp:32`, `llvm/utils/TableGen/Common/SubtargetFeatureInfo.cpp:169`, `llvm/utils/TableGen/Common/SubtargetFeatureInfo.cpp:220` | 是否作为汇编匹配谓词参与生成。 |
| `CondString` | `Predicate` | `code` | `cond` | `derive` | `llvm/utils/TableGen/Common/SubtargetFeatureInfo.cpp:39`, `llvm/utils/TableGen/Common/SubtargetFeatureInfo.cpp:113`, `llvm/utils/TableGen/Common/SubtargetFeatureInfo.cpp:158` | 谓词条件的字符串形式；`SubtargetFeatureInfo` 用其判空。 |
| `RecomputePerFunction` | `Predicate` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/SubtargetFeatureInfo.h:51` | 是否每个函数重新计算该谓词。 |

`stage`：`schema` 仅声明 · `derive` 只被共享派生层读 · `project` 只被本 emitter 读 · `derive+project` 两者都读。

**声明了但本后端不读的字段**

| 字段 | 声明于 | 类型 | 归属 |
| --- | --- | --- | --- |
| `AltNames` | `Register` | `list<string>` | 未知（需查其它后端） |
| `DwarfNumbers` | `Register` | `list<int>` | DWARF 编号；`-gen-register-info` 消费。 |
| `HWEncoding` | `Register` | `bits<16>` | 寄存器编码；`-gen-register-info` 消费。 |
| `PositionOrder` | `Register` | `int` | 寄存器枚举顺序；核心排序器消费。 |
| `RegAltNameIndices` | `Register` | `list<RegAltNameIndex>` | 未知（需查其它后端） |
| `DecoderMethod` | `DAGOperand`, `InstructionEncoding` | `string` | decoder 自定义方法。 |
| `hasCompleteDecoder` | `DAGOperand`, `InstructionEncoding` | `bit` | decoder 完整性标记。 |
| `CoerceFromRegClass` | `RegisterClass` | `RegisterClass` | asm matcher 的寄存器类强制。 |
| `DiagnosticString` | `RegisterClass` | `string` | asm matcher 诊断文本。 |
| `DiagnosticType` | `RegisterClass` | `string` | asm matcher 诊断类型。 |
| `altNameIndex` | `RegisterClass` | `RegAltNameIndex` | 寄存器类别名索引；asm writer 消费。 |
| `DwarfAlias` | `DwarfRegAlias` | `Register` | DWARF 别名；`-gen-register-info` 消费。 |
| `AddedComplexity` | `Instruction` | `int` | isel pattern 优先级；`-gen-dag-isel` 消费。 |
| `AsmMatchConverter` | `Instruction` | `string` | asm matcher 的自定义转换器。 |
| `AsmVariantName` | `Instruction` | `string` | asm variant 选择。 |
| `CodeSize` | `Instruction` | `int` | isel 的 code size 排序；`-gen-dag-isel` 消费。 |
| `GISelMatchGenericTypes` | `Instruction` | `bit` | GlobalISel 泛型类型匹配。 |
| `HasPositionOrder` | `Instruction` | `bit` | asm matcher 的位置排序。 |
| `Itinerary` | `Instruction` | `InstrItinClass` | 旧调度模型；`-gen-instr-info` 不读。 |
| `Pattern` | `Instruction` | `list<dag>` | isel pattern；`-gen-dag-isel` 消费。 |
| `PostEncoderMethod` | `Instruction` | `string` | 编码后处理函数。 |
| `SchedRW` | `Instruction` | `list<SchedReadWrite>` | 新调度模型读写资源。 |
| `TwoOperandAliasConstraint` | `Instruction` | `string` | 两操作数 alias 约束；asm matcher 消费。 |
| `UseLogicalOperandMappings` | `Instruction` | `bit` | 逻辑操作数映射开关；`-gen-instr-info` 消费。 |
| `UseNamedOperandTable` | `Instruction` | `bit` | 命名操作数表开关；`-gen-instr-info` 消费。 |
| `supportsDeactivationSymbol` | `Instruction` | `bit` | 指令级 deactivation 标记；其它后端消费。 |
| `MCOperandPredicate` | `Operand` | `code` | MC operand 谓词；asm writer 消费。 |
| `ParserMatchClass` | `Operand`, `RegisterOperand` | `AsmOperandClass` | asm parser 匹配类。 |
| `Type` | `Operand` | `ValueType` | 操作数 ValueType；isel/asm 消费。 |
| `GIZeroRegister` | `RegisterOperand` | `Register` | GlobalISel 零寄存器。 |
| `PredicateName` | `Predicate` | `string` | 未知（需查其它后端） |

本后端没有硬编码记录名（名字只作为数据/生成标识符使用）。

<!-- END: GENERATED DSL CONTRACT -->

### RVMin `.td` 用法

<!-- BEGIN: GENERATED TD USAGE -->

| 文件:行 | 字段 | 契约状态 |
| --- | --- | --- |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:37` | `Name` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:38` | `PredicateMethod` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:39` | `RenderMethod` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:51` | `PrintMethod` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:52` | `ParserMatchClass` | asm parser 匹配类。 |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:53` | `DecoderMethod` | decoder 自定义方法。 |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:54` | `MCOperandPredicate` | MC operand 谓词；asm writer 消费。 |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:62` | `PrintMethod` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:63` | `ParserMatchClass` | asm parser 匹配类。 |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:64` | `MCOperandPredicate` | MC operand 谓词；asm writer 消费。 |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:76` | `PrintMethod` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:77` | `ParserMatchClass` | asm parser 匹配类。 |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:78` | `MCOperandPredicate` | MC operand 谓词；asm writer 消费。 |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:88` | `PrintMethod` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:89` | `ParserMatchClass` | asm parser 匹配类。 |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:90` | `DecoderMethod` | decoder 自定义方法。 |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:91` | `MCOperandPredicate` | MC operand 谓词；asm writer 消费。 |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:101` | `PrintMethod` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:102` | `ParserMatchClass` | asm parser 匹配类。 |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:103` | `DecoderMethod` | decoder 自定义方法。 |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:104` | `MCOperandPredicate` | MC operand 谓词；asm writer 消费。 |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:116` | `PrintMethod` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:124` | `Name` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:125` | `ParserMethod` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:126` | `DiagnosticType` | asm matcher 诊断类型。 |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:130` | `PrintMethod` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:131` | `ParserMatchClass` | asm parser 匹配类。 |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:132` | `DecoderMethod` | decoder 自定义方法。 |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:139` | `hasSideEffects` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:166` | `hasSideEffects` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:201` | `hasSideEffects` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:212` | `isTerminator` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:223` | `isBranch` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:252` | `hasSideEffects` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:271` | `hasSideEffects` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:475` | `Predicates` | 消费（derive+project） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:528` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:529` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:530` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:531` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:532` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:534` | `TSFlags` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:543` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:544` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:545` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:546` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:547` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:548` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:550` | `TSFlags` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:560` | `Predicates` | 消费（derive+project） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:626` | `Predicates` | 消费（derive+project） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:691` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:692` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:693` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:694` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:695` | `TSFlags` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:696` | `mayLoad` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:697` | `mayStore` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:703` | `OutOperandList` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:704` | `InOperandList` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:705` | `AsmString` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:706` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:707` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:708` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:716` | `OutOperandList` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:717` | `InOperandList` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:718` | `AsmString` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:719` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:720` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:721` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:738` | `Predicates` | 消费（derive+project） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:757` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:758` | `hasSideEffects` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:761` | `Predicates` | 消费（derive+project） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:796` | `Predicates` | 消费（derive+project） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:821` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:822` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:823` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:824` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:825` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:827` | `TSFlags` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:828` | `hasSideEffects` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:829` | `mayLoad` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:830` | `mayStore` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:840` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:841` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:842` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:843` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:844` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:846` | `TSFlags` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:847` | `hasSideEffects` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:848` | `mayLoad` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:849` | `mayStore` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:852` | `Predicates` | 消费（derive+project） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:903` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:904` | `isBarrier` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:908` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:918` | `isReturn` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:919` | `isBarrier` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:920` | `isTerminator` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:921` | `Size` | 消费（derive+project） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:929` | `isBranch` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:930` | `isBarrier` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:931` | `isTerminator` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:932` | `Size` | 消费（derive+project） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:943` | `Defs` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:967` | `isCall` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:976` | `isCall` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:998` | `isTerminator` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:1006` | `isTerminator` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:1018` | `isBranch` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:1019` | `isBarrier` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:1020` | `isTerminator` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:1024` | `isIndirectBranch` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:1025` | `Size` | 消费（derive+project） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:1040` | `Predicates` | 消费（derive+project） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:1042` | `hasSideEffects` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:1046` | `mayLoad` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:1051` | `hasSideEffects` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:1060` | `isCommutable` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:1068` | `isCommutable` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:1081` | `isCommutable` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:1094` | `isCommutable` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:1099` | `isCommutable` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:1106` | `isCommutable` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:1175` | `Predicates` | 消费（derive+project） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:1233` | `Predicates` | 消费（derive+project） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:1235` | `hasSideEffects` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:1238` | `mayLoad` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:1243` | `hasSideEffects` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:1252` | `isCommutable` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:1260` | `isCommutable` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:1273` | `isCommutable` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:1286` | `isCommutable` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:1291` | `isCommutable` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:1298` | `isCommutable` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:1369` | `Predicates` | 消费（derive+project） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:106` | `Namespace` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:107` | `OutOperandList` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:108` | `InOperandList` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:109` | `AsmString` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:110` | `Pattern` | isel pattern；`-gen-dag-isel` 消费。 |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:111` | `Size` | 消费（derive+project） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:116` | `isPseudo` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:118` | `isCodeGenOnly` | 消费（derive+project） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:119` | `Size` | 消费（derive+project） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:120` | `TSFlags` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:134` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:135` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:136` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:137` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:138` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:139` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:141` | `TSFlags` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:155` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:156` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:157` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:158` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:159` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:161` | `TSFlags` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:173` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:174` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:175` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:176` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:177` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:178` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:180` | `TSFlags` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:194` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:195` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:196` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:197` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:198` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:199` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:201` | `TSFlags` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:215` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:216` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:217` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:218` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:219` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:220` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:221` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:222` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:224` | `TSFlags` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:237` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:238` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:239` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:241` | `TSFlags` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:254` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:255` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:256` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:257` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:258` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:259` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:261` | `TSFlags` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:280` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:281` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:282` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:283` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:284` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:285` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:286` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:288` | `TSFlags` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:306` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:307` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:308` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:309` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:310` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:311` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:313` | `TSFlags` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrFormats.td:105` | `Inst` | 自定义字段（需自行保证 name-based 读取方存在） |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:12` | `FallbackRegAltNameIndex` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:17` | `Namespace` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:18` | `HWEncoding` | 寄存器编码；`-gen-register-info` 消费。 |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:19` | `AltNames` | 本后端不读 |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:20` | `DwarfNumbers` | DWARF 编号；`-gen-register-info` 消费。 |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:29` | `Namespace` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:30` | `HWEncoding` | 寄存器编码；`-gen-register-info` 消费。 |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:35` | `RegAltNameIndices` | 本后端不读 |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:120` | `DecoderMethod` | decoder 自定义方法。 |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:128` | `DecoderMethod` | decoder 自定义方法。 |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:148` | `DecoderMethod` | decoder 自定义方法。 |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:176` | `Namespace` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:177` | `HWEncoding` | 寄存器编码；`-gen-register-info` 消费。 |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:178` | `AltNames` | 本后端不读 |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:179` | `DwarfNumbers` | DWARF 编号；`-gen-register-info` 消费。 |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:182` | `RegAltNameIndices` | 本后端不读 |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:237` | `DecoderMethod` | decoder 自定义方法。 |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:248` | `DecoderMethod` | decoder 自定义方法。 |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:255` | `DecoderMethod` | decoder 自定义方法。 |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:260` | `DecoderMethod` | decoder 自定义方法。 |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:16` | `Num` | 自定义字段（需自行保证 name-based 读取方存在） |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:28` | `Num` | 自定义字段（需自行保证 name-based 读取方存在） |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:175` | `Num` | 自定义字段（需自行保证 name-based 读取方存在） |

共 233 处使用；契约外 93 处。
<!-- END: GENERATED TD USAGE -->

`Inst` 是本章的核心输入，但它声明在 RVMin 自己的 `RVMinInst` 上，不在
`Target.td` 的通用 schema 里。生成块里只有它的 `field` 声明显示为
“自定义字段”；`let Inst{...}` 赋值行显示为“不在本后端契约”，这正是
通用契约看不到 target 自定义字段的证据。

<!-- BEGIN: GENERATED PROJECTION MATRIX -->

只列“本后端之外还有读者”或“同名多声明”的字段；`shared` 表示共享派生层也读它。

| 字段 | 声明于 | shared | 其它 `-gen-*` | helper | 同名 |
| --- | --- | --- | --- | --- | --- |
| `Objects` | `RegInfoByHwMode`, `SubRegRangeByHwMode`, `EncodingByHwMode`, `RegClassByHwMode` | ✓ | `instr-info` | — | ⚠ |
| `Size` | `SubRegRange`, `RegisterClass`, `InstructionEncoding`, `Instruction` | ✓ | `asm-matcher`, `callingconv`, `compress-inst-emitter`, `instr-info` | `helper:DecoderEmitter`, `helper:X86RecognizableInstr` | ⚠ |
| `AltNames` | `Register` | — | `asm-matcher`, `asm-writer` | — | — |
| `AsmName` | `Register` | ✓ | `asm-matcher`, `asm-writer` | — | — |
| `DwarfNumbers` | `Register` | — | `register-info` | — | — |
| `HWEncoding` | `Register` | — | `register-info` | — | — |
| `Namespace` | `Register`, `RegisterClass`, `Instruction` | ✓ | `asm-matcher`, `asm-writer`, `emitter`, `fast-isel`, `pseudo-lowering`, `register-info`, `subtarget` | — | ⚠ |
| `RegAltNameIndices` | `Register` | — | `asm-matcher`, `asm-writer` | — | — |
| `DecoderMethod` | `DAGOperand`, `InstructionEncoding` | ✓ | — | — | ⚠ |
| `hasCompleteDecoder` | `DAGOperand`, `InstructionEncoding` | ✓ | — | — | ⚠ |
| `Alignment` | `RegisterClass` | ✓ | — | `helper:X86RecognizableInstr` | — |
| `CoerceFromRegClass` | `RegisterClass` | — | `asm-matcher` | — | — |
| `DiagnosticString` | `RegisterClass` | — | `asm-matcher` | — | — |
| `DiagnosticType` | `RegisterClass` | — | `asm-matcher` | — | — |
| `TSFlags` | `RegisterClass`, `Instruction` | ✓ | `instr-info` | — | ⚠ |
| `DwarfAlias` | `DwarfRegAlias` | — | `register-info` | — | — |
| `DecoderNamespace` | `InstructionEncoding` | ✓ | — | `helper:DecoderEmitter` | — |
| `Predicates` | `InstructionEncoding`, `Instruction` | ✓ | `asm-matcher`, `asm-writer`, `compress-inst-emitter`, `global-isel-combiner`, `instr-docs`, `instr-info`, `macro-fusion-pred`, `subtarget`, `x86-instr-mapping` | `helper:DecoderEmitter`, `helper:X86RecognizableInstr` | ⚠ |
| `AsmMatchConverter` | `Instruction` | — | `asm-matcher` | — | — |
| `AsmString` | `Instruction` | ✓ | `insn-directive-match-table` | — | — |
| `AsmVariantName` | `Instruction` | — | `asm-matcher`, `x86-mnemonic-tables` | — | — |
| `CodeSize` | `Instruction` | — | `dag-isel` | — | — |
| `Constraints` | `Instruction` | ✓ | `instr-docs` | — | — |
| `Defs` | `Instruction` | ✓ | `global-isel-combiner`, `x86-fold-tables`, `x86-instr-mapping` | — | — |
| `EncodingInfos` | `Instruction` | ✓ | `emitter` | `helper:DecoderEmitter` | — |
| `GISelMatchGenericTypes` | `Instruction` | — | `global-isel` | — | — |
| `HasPositionOrder` | `Instruction` | — | `asm-matcher` | — | — |
| `InOperandList` | `Instruction` | ✓ | `insn-directive-match-table`, `x86-fold-tables` | — | — |
| `OutOperandList` | `Instruction` | ✓ | `insn-directive-match-table`, `x86-fold-tables` | — | — |
| `PostEncoderMethod` | `Instruction` | ✓ | `emitter` | — | — |
| `TwoOperandAliasConstraint` | `Instruction` | — | `asm-matcher` | — | — |
| `UseLogicalOperandMappings` | `Instruction` | — | `instr-info` | — | — |
| `UseNamedOperandTable` | `Instruction` | — | `instr-info` | — | — |
| `Uses` | `Instruction` | ✓ | `x86-fold-tables` | — | — |
| `isAsmParserOnly` | `Instruction` | — | `x86-fold-tables`, `x86-instr-mapping` | `helper:DecoderEmitter`, `helper:X86RecognizableInstr` | — |
| `isCodeGenOnly` | `Instruction` | ✓ | — | `helper:DecoderEmitter`, `helper:X86RecognizableInstr` | — |
| `isPseudo` | `Instruction` | ✓ | `searchable-tables`, `x86-fold-tables` | — | — |
| `supportsDeactivationSymbol` | `Instruction` | — | — | `helper:DAGISelMatcherEmitter` | — |
| `AliasOf` | `AdditionalEncoding` | — | — | `helper:DecoderEmitter` | — |
| `EncoderMethod` | `Operand`, `RegisterOperand` | ✓ | — | — | ⚠ |
| `MCOperandPredicate` | `Operand` | — | `asm-writer` | — | — |
| `MIOperandInfo` | `Operand` | ✓ | `instr-info` | `helper:DAGISelMatcherGen` | — |
| `OperandType` | `Operand`, `RegisterOperand` | ✓ | `asm-writer`, `global-isel-combiner` | `helper:X86RecognizableInstr` | ⚠ |
| `ParserMatchClass` | `Operand`, `RegisterOperand` | — | `asm-matcher`, `insn-directive-match-table` | — | ⚠ |
| `PrintMethod` | `Operand`, `RegisterOperand` | ✓ | `asm-writer` | — | ⚠ |
| `Type` | `Operand` | ✓ | `global-isel-combiner`, `x86-fold-tables`, `x86-instr-mapping` | — | — |
| `GIZeroRegister` | `RegisterOperand` | — | `global-isel` | — | — |
| `RegClass` | `RegisterOperand` | ✓ | `asm-matcher`, `fast-isel`, `global-isel`, `instr-info` | `helper:DAGISelMatcherGen`, `helper:X86RecognizableInstr` | — |
| `AssemblerCondDag` | `Predicate` | ✓ | `asm-writer` | — | — |
| `AssemblerMatcherPredicate` | `Predicate` | ✓ | `asm-writer`, `compress-inst-emitter` | `helper:DecoderEmitter` | — |
| `CondString` | `Predicate` | ✓ | `global-isel`, `global-isel-combiner`, `x86-instr-mapping` | — | — |
| `PredicateName` | `Predicate` | — | `asm-matcher` | — | — |

矩阵覆盖 52 个字段（共 127 个）。
<!-- END: GENERATED PROJECTION MATRIX -->

<!-- BEGIN: GENERATED DERIVATIONS -->

Record 里只有声明；这些是共享 C++ 在投影之前对语义的改写。

| 改写 | Schema 键 | 证据 | 说明 |
| --- | --- | --- | --- |
| `encoding collection` | `Instruction` | `llvm/utils/TableGen/Common/CodeGenTarget.cpp:287` | 收集非伪指令，逐条检查 encoding 是否可解码。 |
| `additional encodings` | `AdditionalEncoding` | `llvm/utils/TableGen/DecoderEmitter.cpp:1464` | 把额外 encoding 归并到 `AliasOf` 指向的指令。 |
| `hwmode encoding select` | `EncodingByHwMode` | `llvm/utils/TableGen/DecoderEmitter.cpp:1434` | 按 HwMode 取得 `EncodingByHwMode.Objects` 里的具体 encoding。 |
| `predicate lowering` | `Predicate.AssemblerMatcherPredicate` | `llvm/utils/TableGen/DecoderEmitter.cpp:768`, `llvm/utils/TableGen/Common/CodeGenHwModes.cpp:27` | 把汇编匹配谓词下沉成 MCDecoder 的 feature 检查。 |
<!-- END: GENERATED DERIVATIONS -->

## 构建与验证

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop
T="$W/llvm/utils/TableGen/dsl-surface.py"
DOC="$W/llvm/lib/Target/RVMin/docs/38-disassembler-dsl.md"

# 1. 直接看契约
python3 "$T" --backend disassembler
python3 "$T" --backend disassembler --matrix

# 2. 文档块更新/校验
python3 "$T" --backend disassembler --update "$DOC" \
  --usage "$W/llvm/lib/Target/RVMin/RVMinInstrInfo.td" \
          "$W/llvm/lib/Target/RVMin/RVMinInstrFormats.td" \
          "$W/llvm/lib/Target/RVMin/RVMinRegisterInfo.td"
python3 "$T" --backend disassembler --check "$DOC" \
  --usage "$W/llvm/lib/Target/RVMin/RVMinInstrInfo.td" \
          "$W/llvm/lib/Target/RVMin/RVMinInstrFormats.td" \
          "$W/llvm/lib/Target/RVMin/RVMinRegisterInfo.td"

# 3. 七后端矩阵与字段影响面
python3 "$T" --status-matrix --usage \
  "$W/llvm/lib/Target/RVMin/RVMinRegisterInfo.td" \
  "$W/llvm/lib/Target/RVMin/RVMin.td"
python3 "$T" --effect EncodingInfos

# 4. 全量门禁
"$W/llvm/lib/Target/RVMin/scripts/rvmin-verify-all.sh" "$B/bin"
```

## 预期输出

```text
$ python3 dsl-surface.py --backend disassembler
disassembler: {'roots': 8, 'fields': 127, 'consumed': 96,
               'unused': 31, 'stages': {'schema': 31, 'derive': 88,
               'project': 4, 'derive+project': 4}}

$ python3 dsl-surface.py --backend disassembler --check doc38.md --usage ...
dsl-surface: OK (disassembler: 96/127 fields consumed
              [88 derive, 4 project, 4 both], 0 magic names, 4 derivations)

$ python3 dsl-surface.py --status-matrix --usage ...
dsl-surface: OK (cross-backend status: 13 usages, 7 backends)

$ scripts/rvmin-verify-all.sh "$B/bin"
...
PASS 7/7: TableGen DSL contracts + status matrix + field effect
ALL GATES PASS (run rvmin-run.sh separately for the QEMU check)
```

## 常见坑

1. **一个 `-gen-*` 不等于一个 emitter。** `-gen-disassembler` 是分发器；
   X86 与 WebAssembly 走完全不同的 source set。契约必须声明范围。
2. **`Inst` 是 target-custom。** `--usage` 对 `field bits<32> Inst`
   标“自定义”，对 `let Inst{...}` 标“不在本后端契约”；两者都不是契约
   漏了，而是它们不在 `Target.td`。
3. **`EncodingInfos` 与 `Inst` 互斥。** 前者非空时，后者被忽略；两个字段
   不能同时作为编码来源。
4. **`AdditionalEncoding` 是独立根。** 它不继承 `Instruction`，只继承
   `InstructionEncoding`，通过 `AliasOf` 归并。
5. **`isAsmParserOnly` / `isCodeGenOnly` 是过滤器。** 不是所有
   `Instruction` 都会进入解码表。
6. **`Predicates` 是 `derive+project`。** decoder 直接读它，共享的
   `SubtargetFeatureInfo` 也按同一字段生成 feature 检查。
7. **magic name 为 0。** generic decoder 没有硬编码 dag/record 名字；
   X86 路径的大量字符串比较不在本契约范围内。

## 练习

1. 把 `X86DisassemblerEmitter.cpp` / `X86RecognizableInstr.cpp` 临时加进
   `sources`，观察字段数与 roots 变化，再解释为什么最终不纳入。
2. 用 `--usage` 看 `Inst{...}` 的输出，解释为什么它不在 127 个字段里。
3. 用 `--effect EncodingInfos` 对比 dag-isel / fast-isel / disassembler
   的 stage，说明“同一个字段在不同后端里命运不同”。
4. 给 `--usage` 增加“位段赋值”识别：它应该回答“哪些 bit 由哪个格式类
   写入”，需要解析哪些语法？
5. 比较 asm-matcher 与 disassembler：两者都读 `AsmString`，一个做
   正向匹配，一个做逆向解码；它们在契约里的 stage 有何不同？

## 产物清单

| 文件 | 变更 |
| --- | --- |
| `llvm/utils/TableGen/dsl-surface.py` | 新增 `disassembler` Backend；16 个注解；4 条派生 |
| `llvm/lib/Target/RVMin/docs/38-disassembler-dsl.md` | 新增：本章（四个生成块） |
| `llvm/lib/Target/RVMin/docs/32-cross-backend-status.md` | 生成块升级为七后端 |
| `llvm/lib/Target/RVMin/docs/33-field-effect.md` | 生成块升级为七后端 |
| `llvm/lib/Target/RVMin/docs/36-dag-isel-dsl.md` | 联动说明改为七后端 |
| `llvm/lib/Target/RVMin/docs/37-fast-isel-dsl.md` | 联动说明改为七后端 |
| `llvm/lib/Target/RVMin/docs/README.md` | 新增第 38 章索引行；范围改为 29–38 |
| `llvm/lib/Target/RVMin/scripts/rvmin-verify-all.sh` | 第 7 项增加 disassembler 校验 |
| `llvm/docs/TableGen/BackEnds.md` | 后端列表加 `-gen-disassembler` |
