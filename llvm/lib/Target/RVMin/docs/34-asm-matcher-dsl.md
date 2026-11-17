# 第 34 章：第四个后端——`-gen-asm-matcher` 与非指令 schema

## 目标

第 31 章的“非目标”里写着：

> 不覆盖 `-gen-asm-matcher`（它更大，留作练习）。

本章把这个练习做完。在加入 dag-isel（第 36 章）之前，asm matcher 是四个
后端里 schema 形态最不同的一个：

| 后端 | 入口类 | 字段 | 消费 | derive / project / both | schema | magic | derivations |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `register-info` | 8 | 59 | 44 | 39 / 4 / 1 | 15 | 10 | 11 |
| `instr-info` | 6 | 111 | 85 | 74 / 5 / 6 | 26 | 0 | 8 |
| `asm-writer` | 8 | 130 | 99 | 84 / 9 / 6 | 31 | 2 | 8 |
| `asm-matcher` | **15** | **166** | **142** | **95 / 42 / 5** | 24 | 2 | 7 |

要验证的问题：

> 当一个后端的主要输入不再是 `Instruction`，而是一组“解析器配置记录”
> （`AsmParser`、`AsmParserVariant`、`AsmOperandClass`、`Predicate`）时，
> 同一套 `(declaring class, field)` 契约还能不能描述它？

答案是能，而且 `project` 列（42）第一次成为大头：这些字段不是被共享层派生，
而是**直接发射成 C++ 匹配表**。

本章产出：

- `dsl-surface.py` 增加 `--backend asm-matcher`；
- 契约：15 个记录类入口、166 个字段、142 个被消费、54 行投影矩阵、
  7 条派生改写、2 个硬编码名字；
- 复用 instr-info 注解，补 62 个 asm-matcher 字段语义；
- 第 32 章 status 矩阵与第 33 章 `--effect` 自动扩展为四后端（第 36 章后为五后端，第 37 章后为六后端，第 38 章后为七后端）。

非目标：

- 不修改 `AsmMatcherEmitter` 的行为；
- 不覆盖 `-gen-dag-isel` / `-gen-fast-isel` / `-gen-disassembler`；
- 不把 142 个字段逐个讲一遍；正文只讲新增机制与投影差异。

## 背景：为什么 asm-matcher 是第四个

前三个后端共享同一条“寄存器 + 指令”主干：

```text
Register / RegisterClass ─→ register-info
Instruction / Operand ────→ instr-info
InstAlias / AsmWriter ────→ asm-writer
```

asm-matcher 在主干之外多出四类记录：

```text
AsmParser          ─┐
AsmParserVariant    ├─→ 解析器配置（类名、变体、token 规则）
AsmOperandClass    ─┤
Predicate          ─┴─→ 匹配条件与分类层级

MnemonicAlias / TokenAlias / InstAlias ─→ 匹配前的重写
```

这带来两个直接后果：

1. **入口类从 8 个涨到 15 个。** 多出的 7 个里有 5 个是 asm matcher 自己的
   记录类，`Predicate` 来自 `SubtargetFeatureInfo`，`AsmParser` /
   `AsmParserVariant` 来自 `Target` 的两个列表字段。
2. **`project` 从 9 涨到 42。** `AsmOperandClass` 的 9 个字段全部是 `project`：
   它们只被 emitter 读，并直接变成 `MatchClassKind` 枚举、`isSubclass()`
   偏序和 parse/render 方法调用。

因此本章的重点不是“字段更多”，而是**契约第一次覆盖到非指令记录**。

## 代码地图

| 文件 | 作用 |
| --- | --- |
| `llvm/utils/TableGen/AsmMatcherEmitter.cpp` | 本后端 emitter；约 4600 行 |
| `llvm/utils/TableGen/Common/CodeGenInstAlias.{h,cpp}` | `InstAlias` → `CodeGenInstAlias` |
| `llvm/utils/TableGen/Common/CodeGenInstruction.{h,cpp}` | 指令/操作数模型 |
| `llvm/utils/TableGen/Common/CodeGenRegisters.{h,cpp}` | 寄存器、寄存器类、`getRegBank()` |
| `llvm/utils/TableGen/Common/SubtargetFeatureInfo.{h,cpp}` | `Predicate` → feature bit |
| `llvm/utils/TableGen/Common/CodeGenTarget.{h,cpp}` | `getInstructions()` / `getAsmParser()` |
| `llvm/include/llvm/Target/Target.td` | `AsmParser` / `AsmParserVariant` / `AsmOperandClass` / 别名 schema |
| `llvm/include/llvm/Target/TargetSubtarget.td` | `Predicate` schema |
| `llvm/utils/TableGen/dsl-surface.py` | **本章改造**：第四个 Backend + 66 个注解 |
| `llvm/lib/Target/RVMin/docs/34-asm-matcher-dsl.md` | 本章；四个生成块由脚本生成 |

## 关键实现：第四个后端怎么接进来

### 1. `sources` 是契约边界

```python
"asm-matcher": Backend(
    name="asm-matcher",
    emitter="llvm/utils/TableGen/AsmMatcherEmitter.cpp",
    sources=(
        (emitter, "emitter"),
        CodeGenInstAlias, CodeGenInstruction,
        CodeGenRegisters, CodeGenTarget,
        CodeGenHwModes, InfoByHwMode,
        SubtargetFeatureInfo,
    ),
    ...
)
```

`scan_reads()` 只扫描 `sources` 里的文件。这解释了两个容易误判的现象：

- `PositionOrder` 在本后端是“schema 未消费”：排序器 `LessRecordRegister`
  定义在 `llvm/include/llvm/TableGen/Record.h`，不在 source set 里；
- `Target.InstructionSet` 在 `--usage` 里显示“不在本后端契约”：它被
  `CodeGenTarget::getTargetInstructionSet()` 读，但 `Target` 没有作为根进入
  schema（见第 3 点），source set 边界和 schema 边界是两件事。

### 2. `roots_from` 只放“真正枚举记录”的文件

```python
roots_from=(
    "llvm/utils/TableGen/AsmMatcherEmitter.cpp",
    "llvm/utils/TableGen/Common/CodeGenRegisters.cpp",
    "llvm/utils/TableGen/Common/SubtargetFeatureInfo.cpp",
),
```

- `AsmMatcherEmitter.cpp`：`AsmOperandClass`、`InstAlias`、`TokenAlias`、
  `RegisterOperand`、`MnemonicAlias`；
- `CodeGenRegisters.cpp`：`SubRegIndex`、`Register`、`RegisterClass`、
  `RegisterTuples`、`RegisterCategory`（asm matcher 会调用
  `Target.getRegBank()`，寄存器模型必须完整派生）；
- `SubtargetFeatureInfo.cpp`：`Predicate`。

**故意不放 `CodeGenTarget.cpp`。** 它在构造函数里同时枚举 `Target` 和
`FusionPredicates`；后者与匹配无关，放进来会让契约多出 3 个
`FusionPredicates` 字段。`Target` 级别的两个字段改用 `ambient_fields` 精确引入。

### 3. `extra_roots` 指向“schema 枚举点”，不是本 emitter 的位置

```python
extra_roots=(
    ("Instruction", CodeGenTarget.cpp,
     'getAllDerivedDefinitions("Instruction")'),
    ("Register", CodeGenRegisters.cpp, ...),
    ("RegisterClass", CodeGenRegisters.cpp, ...),
    ("Operand", InstrInfoEmitter.cpp, ...),
    ("AsmParser", CodeGenTarget.cpp, "getAsmParser()"),
    ("AsmParserVariant", CodeGenTarget.cpp, "getAsmParserVariant"),
)
```

`Operand` 甚至是在 `InstrInfoEmitter.cpp` 里枚举的。契约记录的是“这个类在
哪里被枚举”，不是“本后端在哪一行读它”。

### 4. `ambient_fields`：Target 上的两个列表

```python
ambient_fields=(
    ("Target", "AssemblyParsers"),
    ("Target", "AssemblyParserVariants"),
)
```

它们由 `CodeGenTarget::getAsmParser()` / `getAsmParserVariant()` 通过
`getValueAsListOfDefs()` 读取。把 `Target` 整个设为根会拉进几十个无关字段，
所以只声明这两个。

### 5. 注解：复用 instr-info，补 62 个

asm matcher 与 instr-info 共享指令/寄存器语义，脚本仍然“复制 + 覆盖”：

```python
ASM_MATCHER_ANNOTATIONS = dict(INSTR_INFO_ANNOTATIONS)
ASM_MATCHER_ANNOTATIONS.update({
    "AsmParserClassName": "...",
    "SuperClasses": "...",
    "Variant": "...",
    # ... 共 62 个
})
```

新增注解按声明类分组：

| 声明类 | 新增字段数 | 代表字段 |
| --- | --- | --- |
| `AsmParser` | 13 | `AsmParserClassName` / `HasMnemonicFirst` / `OperandParserMethod` |
| `AsmOperandClass` | 9 | `SuperClasses` / `PredicateMethod` / `IsOptional` |
| `Register` | 8 | `AsmName` / `AltNames` / `RegAltNameIndices` |
| `AsmParserVariant` | 6 | `Variant` / `TokenizingCharacters` / `RegisterPrefix` |
| 别名类（`InstAlias`/`MnemonicAlias`/`TokenAlias`） | 6 | `ResultInst` / `FromMnemonic` / `ToToken` |
| `Predicate` | 5 | `AssemblerCondDag` / `PredicateName` |
| `Instruction` | 4 | `AsmMatchConverter` / `TwoOperandAliasConstraint` |
| 子寄存器/寄存器类（共享） | 8 | `SubRegRanges` / `SubRegs` / `CoerceFromRegClass` |
| `Target`（ambient） | 2 | `AssemblyParsers` / `AssemblyParserVariants` |
| `Operand`/`RegisterOperand` | 1 | `ParserMatchClass` |

### 6. 派生改写与硬编码名字

本后端有 7 条派生改写，最重要的是：

- `CodeGenInstAlias::CodeGenInstAlias`：把 `ResultInst` dag 展开成别名操作数；
- `SuperClasses` 展开：把父类列表变成 `isSubclass()` 的偏序；
- `getAllDerivedDefinitions("Register")`：按 `PositionOrder`/名字排序，
  决定寄存器枚举与匹配表顺序；
- `SubtargetFeatureInfo::getAll`：收集 `Predicate` 并分配 feature bit。

硬编码名字只有两个：`Imm`（内置立即数操作数类）和 `MCK_`（生成枚举前缀）。

### 7. 联动：第 32、33 章自动升级（第 38 章后为七后端）

`--status-matrix` 与 `--effect` 都遍历 `BACKENDS`。加入 asm-matcher 后：

- 第 32 章的 status 矩阵多一列 `asm-matcher`；
- 第 33 章的 `TSFlags` 影响面多一行投影，reader 计数也可能变化。

这是设计行为：**后端集合是单一的 SSOT，章节文档只是它的投影。**

## 契约与投影

下面四段由脚本生成，不要手改；改 emitter 或 `.td` 后运行 `--update`。

<!-- BEGIN: GENERATED DSL CONTRACT -->

后端：`gen-asm-matcher` · emitter：`llvm/utils/TableGen/AsmMatcherEmitter.cpp`
CLI：`-match-prefix`
统计：15 个记录类入口 · 166 个字段（schema 24 · derive 95 · project 42 · derive+project 5）。

**记录类入口**

| 记录类 | 定义 | 消费证据 |
| --- | --- | --- |
| `SubRegIndex` | `llvm/include/llvm/Target/Target.td:108` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:1256` |
| `Register` | `llvm/include/llvm/Target/Target.td:173` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:1264` |
| `RegisterClass` | `llvm/include/llvm/Target/Target.td:292` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:1354` |
| `RegisterTuples` | `llvm/include/llvm/Target/Target.td:478` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:1271`, `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:1288` |
| `RegisterCategory` | `llvm/include/llvm/Target/Target.td:506` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:1393` |
| `Instruction` | `llvm/include/llvm/Target/Target.td:608` | `llvm/utils/TableGen/Common/CodeGenTarget.cpp:287` |
| `AsmOperandClass` | `llvm/include/llvm/Target/Target.td:941` | `llvm/utils/TableGen/AsmMatcherEmitter.cpp:1507` |
| `Operand` | `llvm/include/llvm/Target/Target.td:1001` | `llvm/utils/TableGen/InstrInfoEmitter.cpp:456` |
| `RegisterOperand` | `llvm/include/llvm/Target/Target.td:1051` | `llvm/utils/TableGen/AsmMatcherEmitter.cpp:2705` |
| `AsmParser` | `llvm/include/llvm/Target/Target.td:1709` | `llvm/utils/TableGen/Common/CodeGenTarget.cpp:160` |
| `AsmParserVariant` | `llvm/include/llvm/Target/Target.td:1816` | `llvm/utils/TableGen/Common/CodeGenTarget.cpp:171` |
| `TokenAlias` | `llvm/include/llvm/Target/Target.td:1857` | `llvm/utils/TableGen/AsmMatcherEmitter.cpp:1798` |
| `MnemonicAlias` | `llvm/include/llvm/Target/Target.td:1885` | `llvm/utils/TableGen/AsmMatcherEmitter.cpp:3214` |
| `InstAlias` | `llvm/include/llvm/Target/Target.td:1897` | `llvm/utils/TableGen/AsmMatcherEmitter.cpp:1692` |
| `Predicate` | `llvm/include/llvm/Target/TargetSubtarget.td:78` | `llvm/utils/TableGen/Common/SubtargetFeatureInfo.cpp:27` |

**字段契约**（主键是 `(declaring class, field)`；`stage` 表示它停在哪一层，`⚠` 表示同名多声明）

| 字段 | 声明于 | 类型 | 默认值 | stage | 本后端读取点 | 语义 |
| --- | --- | --- | --- | --- | --- | --- |
| `Modes` | `HwModeSelect` | `list<HwMode>` | `Ms` | `derive` | `llvm/utils/TableGen/Common/CodeGenHwModes.cpp:40` | HwMode 列表；与 `Objects` 等长，按 ModeId 绑定。 |
| `RegSize` | `RegInfo` | `int` | `RS` | `derive` | `llvm/utils/TableGen/Common/InfoByHwMode.cpp:105` | `RegInfo` 的寄存器位数。 |
| `SpillAlignment` | `RegInfo` | `int` | `SA` | `derive` | `llvm/utils/TableGen/Common/InfoByHwMode.cpp:107` | `RegInfo` 的 spill slot 对齐。 |
| `SpillSize` | `RegInfo` | `int` | `SS` | `derive` | `llvm/utils/TableGen/Common/InfoByHwMode.cpp:106` | `RegInfo` 的 spill slot 位数。 |
| `Objects` | `RegInfoByHwMode`, `SubRegRangeByHwMode`, `RegClassByHwMode` | `list<RegInfo>` / `list<SubRegRange>` / `list<RegisterClass>` | `Ts` / `RegClasses` | `derive` ⚠ | `llvm/utils/TableGen/Common/CodeGenHwModes.cpp:41`, `llvm/utils/TableGen/Common/CodeGenTarget.cpp:119` | 按 HwMode 绑定的对象列表；`RegisterByHwMode` 是寄存器，`RegInfoByHwMode`/`SubRegRangeByHwMode` 是参数。 |
| `Offset` | `SubRegRange`, `SubRegIndex` | `int` | `offset` | `derive` ⚠ | `llvm/utils/TableGen/Common/InfoByHwMode.cpp:189` | 子寄存器/范围的起始位；`-1` 表示不固定。 |
| `Size` | `SubRegRange`, `SubRegIndex`, `RegisterClass`, `Instruction` | `int` | `size` / `0` | `derive+project` ⚠ | `llvm/utils/TableGen/AsmMatcherEmitter.cpp:584`, `llvm/utils/TableGen/AsmMatcherEmitter.cpp:589`, `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:800`, `llvm/utils/TableGen/Common/InfoByHwMode.cpp:188` | 指令编码字节数；`emitRecord` 写入 MCInstrDesc 的 Size。同名 `RegisterClass.Size` 是 spill size。 |
| `ComposedOf` | `SubRegIndex` | `list<SubRegIndex>` | `[]` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:86` | 复合子寄存器索引的组成列表。 |
| `CoveringSubRegIndices` | `SubRegIndex` | `list<SubRegIndex>` | `[]` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:99` | 覆盖该子寄存器索引的父索引列表。 |
| `Namespace` | `SubRegIndex`, `Register`, `RegisterClass`, `Instruction` | `string` | `""` / `namespace` | `derive+project` ⚠ | `llvm/utils/TableGen/AsmMatcherEmitter.cpp:2873`, `llvm/utils/TableGen/AsmMatcherEmitter.cpp:2994`, `llvm/utils/TableGen/AsmMatcherEmitter.cpp:3021`, `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:419` …(+6) | 指令/寄存器命名空间；`Target.getInstNamespace()` 决定生成 enum 的 C++ namespace。 |
| `SubRegRanges` | `SubRegIndex` | `SubRegRangeByHwMode` | `（未设默认值）` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:61` | 按 HwMode 的子寄存器范围。 |
| `Aliases` | `Register` | `list<Register>` | `[]` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:193` | 寄存器别名列表；共享寄存器模型合并别名。 |
| `AltNames` | `Register` | `list<string>` | `altNames` | `project` | `llvm/utils/TableGen/AsmMatcherEmitter.cpp:3030` | 寄存器备用汇编名；asm matcher 生成 alt-name 匹配表。 |
| `AsmName` | `Register` | `string` | `n` | `derive+project` | `llvm/utils/TableGen/AsmMatcherEmitter.cpp:1340`, `llvm/utils/TableGen/AsmMatcherEmitter.cpp:1351`, `llvm/utils/TableGen/AsmMatcherEmitter.cpp:2996`, `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:1307` | 寄存器主汇编名；生成寄存器名匹配表。 |
| `CostPerUse` | `Register` | `list<int>` | `[0]` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:166`, `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:668` | 每寄存器使用成本序列；共享寄存器模型消费。 |
| `CoveredBySubRegs` | `Register` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:167` | 值由子寄存器决定；共享寄存器模型消费。 |
| `RegAltNameIndices` | `Register` | `list<RegAltNameIndex>` | `[]` | `project` | `llvm/utils/TableGen/AsmMatcherEmitter.cpp:3031` | 与 `AltNames` 一一对应的别名索引。 |
| `SubRegIndices` | `Register`, `RegisterTuples` | `list<SubRegIndex>` | `[]` / `Indices` | `derive` ⚠ | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:175`, `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:632` | 与 `SubRegs` 一一对应的子寄存器索引。 |
| `SubRegs` | `Register`, `RegisterTuples` | `list<Register>` / `list<dag>` | `[]` / `Regs` | `derive` ⚠ | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:176`, `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:634` | 子寄存器列表；共享寄存器模型消费。 |
| `isArtificial` | `Register` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:170` | 合成寄存器标记；不参与用户可见匹配。 |
| `isConstant` | `Register` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:168` | 常量寄存器标记；共享寄存器模型消费。 |
| `OperandNamespace` | `DAGOperand` | `string` | `"MCOI"` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:89`, `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:94` | MC operand 类型所在 namespace；与 `OperandType` 一起构成完整类型名。 |
| `Alignment` | `RegisterClass` | `int` | `alignment` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:807` | 寄存器类 spill 对齐（bit）；写入 `RegClassInfo::SpillAlignment`。 |
| `AllocationPriority` | `RegisterClass` | `int` | `0` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:818` | 寄存器分配优先级；越高越先分配。 |
| `AltOrderSelect` | `RegisterClass` | `code` | `[{}]` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:813` | `code` 注入点；非空时生成替代分配顺序选择函数。 |
| `AltOrders` | `RegisterClass` | `list<dag>` | `[]` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:764` | 备用分配顺序列表；与 `AltOrderSelect` 配合。 |
| `BaseClassOrder` | `RegisterClass` | `int` | `?` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.h:619` | 寄存器类在继承层次中的顺序；影响类排序。 |
| `CoerceFromRegClass` | `RegisterClass` | `RegisterClass` | `?` | `project` | `llvm/utils/TableGen/AsmMatcherEmitter.cpp:1348` | 匹配时把该寄存器类强制转换到另一个类。 |
| `CopyCost` | `RegisterClass` | `int` | `1` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:811` | 寄存器拷贝代价；8-bit，>255 fatal，负值实际按 uint8_t 截断。 |
| `DiagnosticString` | `RegisterClass`, `AsmOperandClass` | `string` | `""` | `project` ⚠ | `llvm/utils/TableGen/AsmMatcherEmitter.cpp:1456`, `llvm/utils/TableGen/AsmMatcherEmitter.cpp:1565` | 参数化诊断文本；用于非法操作数报错。 |
| `DiagnosticType` | `RegisterClass`, `AsmOperandClass` | `string` | `""` | `project` ⚠ | `llvm/utils/TableGen/AsmMatcherEmitter.cpp:1452`, `llvm/utils/TableGen/AsmMatcherEmitter.cpp:1562` | 匹配失败时使用的诊断类型；`AsmParser` 侧映射成文本。 |
| `GeneratePressureSet` | `RegisterClass` | `bit` | `true` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:758` | 是否生成寄存器压力集。 |
| `GlobalPriority` | `RegisterClass` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:823` | 全局分配优先级。 |
| `MemberList` | `RegisterClass` | `dag` | `regList` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:1249` | 不是普通 list：经 SetTheory field expander 展开，决定寄存器类成员集与默认分配序。 |
| `RegInfos` | `RegisterClass` | `RegInfoByHwMode` | `（未设默认值）` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:798` | 按 HwMode 的 `{RegSize,SpillSize,SpillAlignment}` 三元组；覆盖默认模式值。 |
| `RegTypes` | `RegisterClass` | `list<ValueType>` | `regTypes` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:759` | 寄存器类支持的 MVT 列表；`Size==0` 时用于推断位宽。 |
| `SpillStackID` | `RegisterClass` | `int` | `0` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:814` | spill stack slot 的自定义 ID；影响 frame index 分配。 |
| `TSFlags` | `RegisterClass`, `Instruction` | `bits<8>` / `bits<64>` | `0` | `derive` ⚠ | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:825` | 目标自定义标志位；`emitRecord` 校验位宽并写入 MCInstrDesc 的 TSFlags。同名 `RegisterClass.TSFlags` 是寄存器类标志。 |
| `Weight` | `RegisterClass` | `int` | `?` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:920` | 寄存器类的压力权重；影响压力集计算。 |
| `isAllocatable` | `RegisterClass` | `bit` | `true` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:812` | 是否可分配；决定 `InAllocatableClassTable`。 |
| `RegAsmNames` | `RegisterTuples` | `list<string>` | `RegNames` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:651` | `RegisterTuples` 展开时覆盖每个合成寄存器的 `AsmName`。 |
| `Classes` | `RegisterCategory` | `list<RegisterClass>` | `classes` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:1235` | `RegisterCategory` 包含的寄存器类；共享寄存器模型用于分类。 |
| `AsmMatchConverter` | `Instruction` | `string` | `""` | `project` | `llvm/utils/TableGen/AsmMatcherEmitter.cpp:1138`, `llvm/utils/TableGen/AsmMatcherEmitter.cpp:2193` | 自定义汇编匹配转换函数名；非空时替代 dag 生成的转换代码。 |
| `AsmString` | `Instruction`, `InstAlias` | `string` | `""` / `Asm` | `derive` ⚠ | `llvm/utils/TableGen/Common/CodeGenInstAlias.cpp:137`, `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:420` | 汇编模板；`CodeGenInstruction` 保存，供 asm writer/matcher 使用；本后端只解析不直接发射。 |
| `AsmVariantName` | `Instruction`, `MnemonicAlias`, `InstAlias` | `string` | `""` / `VariantName` | `project` ⚠ | `llvm/utils/TableGen/AsmMatcherEmitter.cpp:1673`, `llvm/utils/TableGen/AsmMatcherEmitter.cpp:1701`, `llvm/utils/TableGen/AsmMatcherEmitter.cpp:3138` | 所属汇编变体；非空时只在同名变体下参与匹配。 |
| `Constraints` | `Instruction` | `string` | `""` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:480` | 内联汇编约束字符串；`ParseConstraints` 为每个操作数填约束位。 |
| `Defs` | `Instruction` | `list<Register>` | `[]` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:472` | 隐式定义寄存器列表；合并进 `ImplicitDefs`，发射到 `ImplicitLists`。 |
| `FastISelShouldIgnore` | `Instruction` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:453` | FastISel 忽略该指令；只保存在 `CodeGenInstruction`，不进入 MCInstrDesc。 |
| `HasPositionOrder` | `Instruction` | `bit` | `false` | `project` | `llvm/utils/TableGen/AsmMatcherEmitter.cpp:681`, `llvm/utils/TableGen/AsmMatcherEmitter.cpp:683` | 指令是否带位置排序；影响 alias/匹配排序。 |
| `InOperandList` | `Instruction` | `dag` | `（未设默认值）` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:43` | 输入操作数 dag；展开成 use 列表，参与操作数编号与 `MIOperandInfo` 解析。 |
| `OutOperandList` | `Instruction` | `dag` | `（未设默认值）` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:29` | 输出操作数 dag；`CodeGenInstruction` 展开成 def 列表，决定 `NumDefs`。 |
| `Predicates` | `Instruction`, `MnemonicAlias`, `InstAlias` | `list<Predicate>` | `[]` | `derive+project` ⚠ | `llvm/utils/TableGen/AsmMatcherEmitter.cpp:991`, `llvm/utils/TableGen/AsmMatcherEmitter.cpp:3115`, `llvm/utils/TableGen/Common/CodeGenHwModes.cpp:27` | 指令谓词列表；`computeRequiredFeatures` 用它编码每个 opcode 的 feature bitset。 |
| `TwoOperandAliasConstraint` | `Instruction` | `string` | `""` | `project` | `llvm/utils/TableGen/AsmMatcherEmitter.cpp:1771` | 两操作数 alias 的额外约束表达式；asm matcher 生成检查。 |
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
| `isAuthenticated` | `Instruction` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:455` | MCID::Authenticated。 |
| `isBarrier` | `Instruction` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:432` | MCID::Barrier；控制流不能 fall through。 |
| `isBitcast` | `Instruction` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:430` | MCID::Bitcast。 |
| `isBranch` | `Instruction` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:425` | MCID::Branch。 |
| `isCall` | `Instruction` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:433` | MCID::Call。 |
| `isCodeGenOnly` | `Instruction` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:469` | 仅 codegen 使用，汇编器不可见；影响 asm matcher 与 MCID 标志。 |
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
| `DefaultMethod` | `AsmOperandClass` | `string` | `?` | `project` | `llvm/utils/TableGen/AsmMatcherEmitter.cpp:1578` | 可选操作数的默认构造方法名；未设时由 `Name` 派生 `default<Name>Operands`。 |
| `IsOptional` | `AsmOperandClass` | `bit` | `false` | `project` | `llvm/utils/TableGen/AsmMatcherEmitter.cpp:1573` | 该操作数类是否可省略；匹配结束时允许缺少。 |
| `Name` | `AsmOperandClass`, `AsmParserVariant` | `string` | `?` / `""` | `project` ⚠ | `llvm/utils/TableGen/AsmMatcherEmitter.cpp:1534`, `llvm/utils/TableGen/AsmMatcherEmitter.cpp:1659`, `llvm/utils/TableGen/AsmMatcherEmitter.cpp:1842`, `llvm/utils/TableGen/AsmMatcherEmitter.cpp:3224` | 操作数类的 `MCK_<Name>` 枚举名与默认方法名前缀；`AsmParserVariant` 中则是变体名，用于过滤 `AsmVariantName`。 |
| `ParserMethod` | `AsmOperandClass` | `string` | `?` | `project` | `llvm/utils/TableGen/AsmMatcherEmitter.cpp:1557` | 自定义操作数解析方法名；非空时生成 parse switch 分支。 |
| `PredicateMethod` | `AsmOperandClass` | `string` | `?` | `project` | `llvm/utils/TableGen/AsmMatcherEmitter.cpp:1539` | 操作数类判定方法名；未设时由 `Name` 派生 `is<Name>`。 |
| `RenderMethod` | `AsmOperandClass` | `string` | `?` | `project` | `llvm/utils/TableGen/AsmMatcherEmitter.cpp:1548` | 操作数写回 MCInst 的方法名；未设时派生 `add<Name>Operands`。 |
| `SuperClasses` | `AsmOperandClass` | `list<AsmOperandClass>` | `[]` | `project` | `llvm/utils/TableGen/AsmMatcherEmitter.cpp:1520` | 操作数类的父类列表；决定分类层级与匹配优先级。 |
| `EncoderMethod` | `Operand`, `RegisterOperand` | `string` | `""` | `derive` ⚠ | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:90`, `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:96`, `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:181` | 自定义编码函数名；`CodeGenInstruction` 保存，供 encoder 调用。 |
| `MIOperandInfo` | `Operand` | `dag` | `(ops)` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstAlias.cpp:32`, `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:97` | 复合操作数的子操作数 dag；`CollectOperandInfo` 用它展开 MC operand 列表。 |
| `OperandType` | `Operand`, `RegisterOperand` | `string` | `"OPERAND_UNKNOWN"` / `"OPERAND_REGISTER"` | `derive` ⚠ | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:88`, `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:93` | MC operand 的 C++ 类型名；`CodeGenInstruction` 存入 `OperandType`。 |
| `ParserMatchClass` | `Operand`, `RegisterOperand` | `AsmOperandClass` | `ImmAsmOperand` / `（未设默认值）` | `project` ⚠ | `llvm/utils/TableGen/AsmMatcherEmitter.cpp:1227`, `llvm/utils/TableGen/AsmMatcherEmitter.cpp:1261`, `llvm/utils/TableGen/AsmMatcherEmitter.cpp:1841`, `llvm/utils/TableGen/AsmMatcherEmitter.cpp:2706` | 操作数对应的 `AsmOperandClass`；决定分类与自定义解析。 |
| `PrintMethod` | `Operand`, `RegisterOperand` | `string` | `"printOperand"` / `pm` | `derive` ⚠ | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:87`, `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:92` | 自定义打印函数名；`CodeGenInstruction` 保存，供 asm writer 调用。 |
| `RegClass` | `RegisterOperand` | `RegisterClassLike` | `regclass` | `derive+project` | `llvm/utils/TableGen/AsmMatcherEmitter.cpp:435`, `llvm/utils/TableGen/AsmMatcherEmitter.cpp:1240`, `llvm/utils/TableGen/AsmMatcherEmitter.cpp:2713`, `llvm/utils/TableGen/Common/CodeGenTarget.cpp:274` …(+1) | 操作数绑定的寄存器类；解析为 `RegisterClass` 并用于寄存器操作数信息。 |
| `AllowDuplicateRegisterNames` | `AsmParser` | `bit` | `false` | `project` | `llvm/utils/TableGen/AsmMatcherEmitter.cpp:3008`, `llvm/utils/TableGen/AsmMatcherEmitter.cpp:3025` | 允许寄存器名重复；控制是否仍生成匹配函数并去重。 |
| `AsmParserClassName` | `AsmParser` | `string` | `"AsmParser"` | `project` | `llvm/utils/TableGen/AsmMatcherEmitter.cpp:2600`, `llvm/utils/TableGen/AsmMatcherEmitter.cpp:2625`, `llvm/utils/TableGen/AsmMatcherEmitter.cpp:2658`, `llvm/utils/TableGen/AsmMatcherEmitter.cpp:2838` …(+2) | 生成解析器的类名后缀；默认 `AsmParser`。 |
| `AsmParserInstCleanup` | `AsmParser` | `string` | `""` | `project` | `llvm/utils/TableGen/AsmMatcherEmitter.cpp:4510` | 每条指令匹配成功后调用的目标自定义清理函数；空则不生成。 |
| `CallCustomParserForAllOperands` | `AsmParser` | `bit` | `false` | `project` | `llvm/utils/TableGen/AsmMatcherEmitter.cpp:1604` | 对所有操作数调用自定义 parser，而不仅是声明了 `ParserMethod` 的类。 |
| `EmitTokenDiagnosticTypes` | `AsmParser` | `bit` | `false` | `project` | `llvm/utils/TableGen/AsmMatcherEmitter.cpp:1215`, `llvm/utils/TableGen/AsmMatcherEmitter.cpp:2827`, `llvm/utils/TableGen/AsmMatcherEmitter.cpp:3838` | 为 token 匹配失败生成带诊断类型的返回值。 |
| `HasMnemonicFirst` | `AsmParser` | `bit` | `true` | `project` | `llvm/utils/TableGen/AsmMatcherEmitter.cpp:1640`, `llvm/utils/TableGen/AsmMatcherEmitter.cpp:3704` | 输入是否总是助记符在前；决定匹配表布局。 |
| `OperandParserMethod` | `AsmParser` | `string` | `""` | `project` | `llvm/utils/TableGen/AsmMatcherEmitter.cpp:3410` | 自定义操作数解析入口方法名；生成调用点使用。 |
| `PreferSmallerInstructions` | `AsmParser` | `bit` | `false` | `project` | `llvm/utils/TableGen/AsmMatcherEmitter.cpp:398` | 同等匹配时优先较小（`Size` 小）的指令。 |
| `PrioritizeFeatureInMultiMismatchFallback` | `AsmParser` | `bit` | `false` | `project` | `llvm/utils/TableGen/AsmMatcherEmitter.cpp:3709` | 多重不匹配回退时优先报告 feature 冲突。 |
| `ReportMultipleNearMisses` | `AsmParser` | `bit` | `false` | `project` | `llvm/utils/TableGen/AsmMatcherEmitter.cpp:1642`, `llvm/utils/TableGen/AsmMatcherEmitter.cpp:3707` | 生成 near-miss 候选列表，而不是只报第一个失败。 |
| `ShouldEmitMatchRegisterAltName` | `AsmParser` | `bit` | `false` | `project` | `llvm/utils/TableGen/AsmMatcherEmitter.cpp:3792` | 是否生成备用寄存器名匹配函数。 |
| `ShouldEmitMatchRegisterAltNameWithIndex` | `AsmParser` | `bit` | `false` | `project` | `llvm/utils/TableGen/AsmMatcherEmitter.cpp:3023`, `llvm/utils/TableGen/AsmMatcherEmitter.cpp:3793` | 是否按 AltNameIndex 生成逐索引匹配函数，不做回退。 |
| `ShouldEmitMatchRegisterName` | `AsmParser` | `bit` | `true` | `project` | `llvm/utils/TableGen/AsmMatcherEmitter.cpp:3789` | 是否生成主寄存器名匹配函数。 |
| `BreakCharacters` | `AsmParserVariant` | `string` | `""` | `project` | `llvm/utils/TableGen/AsmMatcherEmitter.cpp:1658` | 开始新标识符的字符集合；tokenizer 使用。 |
| `CommentDelimiter` | `AsmParserVariant` | `string` | `""` | `project` | `llvm/utils/TableGen/AsmMatcherEmitter.cpp:1651` | 汇编模板中的注释定界符。 |
| `RegisterPrefix` | `AsmParserVariant` | `string` | `""` | `project` | `llvm/utils/TableGen/AsmMatcherEmitter.cpp:1653` | 寄存器 token 前缀；用于自动识别寄存器。 |
| `SeparatorCharacters` | `AsmParserVariant` | `string` | `" \t,"` | `project` | `llvm/utils/TableGen/AsmMatcherEmitter.cpp:1657` | 不属于 token 的分隔字符（默认空格/制表/逗号）。 |
| `TokenizingCharacters` | `AsmParserVariant` | `string` | `"[]*!"` | `project` | `llvm/utils/TableGen/AsmMatcherEmitter.cpp:1655` | 独立成 token 的字符集合（默认 `[]*!`）。 |
| `Variant` | `AsmParserVariant` | `int` | `0` | `project` | `llvm/utils/TableGen/AsmMatcherEmitter.cpp:1660`, `llvm/utils/TableGen/AsmMatcherEmitter.cpp:3223`, `llvm/utils/TableGen/AsmMatcherEmitter.cpp:3491`, `llvm/utils/TableGen/AsmMatcherEmitter.cpp:3551` …(+2) | 汇编变体编号；写入 `AsmVariantID` 并参与匹配比较。 |
| `FromToken` | `TokenAlias` | `string` | `From` | `project` | `llvm/utils/TableGen/AsmMatcherEmitter.cpp:1799` | `TokenAlias` 的源 token 字面量。 |
| `ToToken` | `TokenAlias` | `string` | `To` | `project` | `llvm/utils/TableGen/AsmMatcherEmitter.cpp:1800` | `TokenAlias` 的目标 token 字面量；建立 token 子类关系。 |
| `FromMnemonic` | `MnemonicAlias` | `string` | `From` | `project` | `llvm/utils/TableGen/AsmMatcherEmitter.cpp:3141` | `MnemonicAlias` 的源助记符；生成助记符重写表。 |
| `ToMnemonic` | `MnemonicAlias` | `string` | `To` | `project` | `llvm/utils/TableGen/AsmMatcherEmitter.cpp:3165`, `llvm/utils/TableGen/AsmMatcherEmitter.cpp:3166`, `llvm/utils/TableGen/AsmMatcherEmitter.cpp:3178`, `llvm/utils/TableGen/AsmMatcherEmitter.cpp:3185` …(+1) | `MnemonicAlias` 的目标助记符；重写后的规范形式。 |
| `ResultInst` | `InstAlias` | `dag` | `Result` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstAlias.cpp:136` | 别名展开的目标指令 dag；`CodeGenInstAlias` 解析成 MCInst 操作数。 |
| `UseInstAsmMatchConverter` | `InstAlias` | `bit` | `true` | `project` | `llvm/utils/TableGen/AsmMatcherEmitter.cpp:590` | 是否复用目标指令的 `AsmMatchConverter`。 |
| `AssemblyParserVariants` | `Target` | `list<AsmParserVariant>` | `[DefaultAsmParserVariant]` | `derive` | `llvm/utils/TableGen/Common/CodeGenTarget.cpp:176`, `llvm/utils/TableGen/Common/CodeGenTarget.cpp:187` | 汇编变体记录列表；决定 `Variant` 匹配循环。 |
| `AssemblyParsers` | `Target` | `list<AsmParser>` | `[DefaultAsmParser]` | `derive` | `llvm/utils/TableGen/Common/CodeGenTarget.cpp:164` | 目标选用的 `AsmParser` 记录列表；`-asmparsernum` 选择。 |
| `AssemblerCondDag` | `Predicate` | `dag` | `（未设默认值）` | `derive` | `llvm/utils/TableGen/Common/SubtargetFeatureInfo.cpp:181`, `llvm/utils/TableGen/Common/SubtargetFeatureInfo.cpp:203`, `llvm/utils/TableGen/Common/SubtargetFeatureInfo.cpp:224` | 汇编器谓词的 dag 条件；展开成 feature 检查代码。 |
| `AssemblerMatcherPredicate` | `Predicate` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/SubtargetFeatureInfo.cpp:32`, `llvm/utils/TableGen/Common/SubtargetFeatureInfo.cpp:169`, `llvm/utils/TableGen/Common/SubtargetFeatureInfo.cpp:220` | 是否作为汇编匹配谓词参与生成。 |
| `CondString` | `Predicate` | `code` | `cond` | `derive` | `llvm/utils/TableGen/Common/SubtargetFeatureInfo.cpp:39`, `llvm/utils/TableGen/Common/SubtargetFeatureInfo.cpp:113`, `llvm/utils/TableGen/Common/SubtargetFeatureInfo.cpp:158` | 谓词条件的字符串形式；`SubtargetFeatureInfo` 用其判空。 |
| `PredicateName` | `Predicate` | `string` | `""` | `project` | `llvm/utils/TableGen/AsmMatcherEmitter.cpp:3099` | 生成 `getSubtargetFeatureName` 时使用的谓词名。 |
| `RecomputePerFunction` | `Predicate` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/SubtargetFeatureInfo.h:51` | 是否每个函数重新计算该谓词。 |

`stage`：`schema` 仅声明 · `derive` 只被共享派生层读 · `project` 只被本 emitter 读 · `derive+project` 两者都读。

**声明了但本后端不读的字段**

| 字段 | 声明于 | 类型 | 归属 |
| --- | --- | --- | --- |
| `DwarfNumbers` | `Register` | `list<int>` | DWARF 编号；`-gen-register-info` 消费。 |
| `HWEncoding` | `Register` | `bits<16>` | 寄存器编码；`-gen-register-info` 消费。 |
| `PositionOrder` | `Register`, `RegisterTuples` | `int` | 寄存器枚举顺序；核心排序器消费。 |
| `DecoderMethod` | `DAGOperand`, `InstructionEncoding` | `string` | decoder 自定义方法。 |
| `hasCompleteDecoder` | `DAGOperand`, `InstructionEncoding` | `bit` | decoder 完整性标记。 |
| `altNameIndex` | `RegisterClass` | `RegAltNameIndex` | 寄存器类别名索引；asm writer 消费。 |
| `DwarfAlias` | `DwarfRegAlias` | `Register` | DWARF 别名；`-gen-register-info` 消费。 |
| `DecoderNamespace` | `InstructionEncoding` | `string` | decoder 生成 namespace。 |
| `AddedComplexity` | `Instruction` | `int` | isel pattern 优先级；`-gen-dag-isel` 消费。 |
| `CodeSize` | `Instruction` | `int` | isel 的 code size 排序；`-gen-dag-isel` 消费。 |
| `EncodingInfos` | `Instruction` | `EncodingByHwMode` | 按 HwMode 的编码；encoder/decoder 消费。 |
| `GISelMatchGenericTypes` | `Instruction` | `bit` | GlobalISel 泛型类型匹配。 |
| `Itinerary` | `Instruction` | `InstrItinClass` | 旧调度模型；`-gen-instr-info` 不读。 |
| `Pattern` | `Instruction` | `list<dag>` | isel pattern；`-gen-dag-isel` 消费。 |
| `PostEncoderMethod` | `Instruction` | `string` | 编码后处理函数。 |
| `SchedRW` | `Instruction` | `list<SchedReadWrite>` | 新调度模型读写资源。 |
| `UseLogicalOperandMappings` | `Instruction` | `bit` | 逻辑操作数映射开关；`-gen-instr-info` 消费。 |
| `UseNamedOperandTable` | `Instruction` | `bit` | 命名操作数表开关；`-gen-instr-info` 消费。 |
| `isAsmParserOnly` | `Instruction` | `bit` | 仅汇编器可见的标记；本后端的匹配表不直接读它。 |
| `supportsDeactivationSymbol` | `Instruction` | `bit` | 指令级 deactivation 标记；其它后端消费。 |
| `MCOperandPredicate` | `Operand` | `code` | MC operand 谓词；asm writer 消费。 |
| `Type` | `Operand` | `ValueType` | 操作数 ValueType；isel/asm 消费。 |
| `GIZeroRegister` | `RegisterOperand` | `Register` | GlobalISel 零寄存器。 |
| `EmitPriority` | `InstAlias` | `int` | 别名打印优先级；`-gen-asm-writer` 消费，asm matcher 不读。 |

**名字即 API**（不经过字段查找的硬编码名字）

| 名字 | 出现位置 | 作用 |
| --- | --- | --- |
| `Imm` | `llvm/utils/TableGen/AsmMatcherEmitter.cpp:1842` | 内置立即数操作数类；`ParserMatchClass` 为 Imm 时走立即数分类。 |
| `MCK_` | `llvm/utils/TableGen/AsmMatcherEmitter.cpp:1202`, `llvm/utils/TableGen/AsmMatcherEmitter.cpp:1446`, `llvm/utils/TableGen/AsmMatcherEmitter.cpp:1475` …(+2) | 生成的操作数类枚举名前缀；`MatchClassKind` 的基本单位。 |

<!-- END: GENERATED DSL CONTRACT -->

<!-- BEGIN: GENERATED PROJECTION MATRIX -->

只列“本后端之外还有读者”或“同名多声明”的字段；`shared` 表示共享派生层也读它。

| 字段 | 声明于 | shared | 其它 `-gen-*` | helper | 同名 |
| --- | --- | --- | --- | --- | --- |
| `Objects` | `RegInfoByHwMode`, `SubRegRangeByHwMode`, `RegClassByHwMode` | ✓ | `instr-info` | — | ⚠ |
| `Offset` | `SubRegRange`, `SubRegIndex` | ✓ | — | — | ⚠ |
| `Size` | `SubRegRange`, `SubRegIndex`, `RegisterClass`, `Instruction` | ✓ | `callingconv`, `compress-inst-emitter`, `instr-info` | `helper:DecoderEmitter`, `helper:X86RecognizableInstr` | ⚠ |
| `Namespace` | `SubRegIndex`, `Register`, `RegisterClass`, `Instruction` | ✓ | `asm-writer`, `emitter`, `fast-isel`, `pseudo-lowering`, `register-info`, `subtarget` | — | ⚠ |
| `AltNames` | `Register` | — | `asm-writer` | — | — |
| `AsmName` | `Register` | ✓ | `asm-writer` | — | — |
| `DwarfNumbers` | `Register` | — | `register-info` | — | — |
| `HWEncoding` | `Register` | — | `register-info` | — | — |
| `PositionOrder` | `Register`, `RegisterTuples` | — | — | — | ⚠ |
| `RegAltNameIndices` | `Register` | — | `asm-writer` | — | — |
| `SubRegIndices` | `Register`, `RegisterTuples` | ✓ | — | — | ⚠ |
| `SubRegs` | `Register`, `RegisterTuples` | ✓ | — | — | ⚠ |
| `DecoderMethod` | `DAGOperand`, `InstructionEncoding` | ✓ | — | — | ⚠ |
| `hasCompleteDecoder` | `DAGOperand`, `InstructionEncoding` | ✓ | — | — | ⚠ |
| `Alignment` | `RegisterClass` | ✓ | — | `helper:X86RecognizableInstr` | — |
| `DiagnosticString` | `RegisterClass`, `AsmOperandClass` | — | — | — | ⚠ |
| `DiagnosticType` | `RegisterClass`, `AsmOperandClass` | — | — | — | ⚠ |
| `TSFlags` | `RegisterClass`, `Instruction` | ✓ | `instr-info` | — | ⚠ |
| `DwarfAlias` | `DwarfRegAlias` | — | `register-info` | — | — |
| `DecoderNamespace` | `InstructionEncoding` | ✓ | — | `helper:DecoderEmitter` | — |
| `AsmString` | `Instruction`, `InstAlias` | ✓ | `insn-directive-match-table` | — | ⚠ |
| `AsmVariantName` | `Instruction`, `MnemonicAlias`, `InstAlias` | — | `x86-mnemonic-tables` | — | ⚠ |
| `CodeSize` | `Instruction` | — | `dag-isel` | — | — |
| `Constraints` | `Instruction` | ✓ | `instr-docs` | — | — |
| `Defs` | `Instruction` | ✓ | `global-isel-combiner`, `x86-fold-tables`, `x86-instr-mapping` | — | — |
| `EncodingInfos` | `Instruction` | ✓ | `emitter` | `helper:DecoderEmitter` | — |
| `GISelMatchGenericTypes` | `Instruction` | — | `global-isel` | — | — |
| `InOperandList` | `Instruction` | ✓ | `insn-directive-match-table`, `x86-fold-tables` | — | — |
| `OutOperandList` | `Instruction` | ✓ | `insn-directive-match-table`, `x86-fold-tables` | — | — |
| `PostEncoderMethod` | `Instruction` | ✓ | `emitter` | — | — |
| `Predicates` | `Instruction`, `MnemonicAlias`, `InstAlias` | ✓ | `asm-writer`, `compress-inst-emitter`, `global-isel-combiner`, `instr-docs`, `instr-info`, `macro-fusion-pred`, `subtarget`, `x86-instr-mapping` | `helper:DecoderEmitter`, `helper:X86RecognizableInstr` | ⚠ |
| `UseLogicalOperandMappings` | `Instruction` | — | `instr-info` | — | — |
| `UseNamedOperandTable` | `Instruction` | — | `instr-info` | — | — |
| `Uses` | `Instruction` | ✓ | `x86-fold-tables` | — | — |
| `isAsmParserOnly` | `Instruction` | — | `x86-fold-tables`, `x86-instr-mapping` | `helper:DecoderEmitter`, `helper:X86RecognizableInstr` | — |
| `isCodeGenOnly` | `Instruction` | ✓ | — | `helper:DecoderEmitter`, `helper:X86RecognizableInstr` | — |
| `isPseudo` | `Instruction` | ✓ | `searchable-tables`, `x86-fold-tables` | — | — |
| `supportsDeactivationSymbol` | `Instruction` | — | — | `helper:DAGISelMatcherEmitter` | — |
| `Name` | `AsmOperandClass`, `AsmParserVariant` | ✓ | `dxil-operation`, `insn-directive-match-table`, `instr-docs`, `opt-rst`, `register-bank`, `subtarget` | — | ⚠ |
| `EncoderMethod` | `Operand`, `RegisterOperand` | ✓ | — | — | ⚠ |
| `MCOperandPredicate` | `Operand` | — | `asm-writer` | — | — |
| `MIOperandInfo` | `Operand` | ✓ | `instr-info` | `helper:DAGISelMatcherGen` | — |
| `OperandType` | `Operand`, `RegisterOperand` | ✓ | `asm-writer`, `global-isel-combiner` | `helper:X86RecognizableInstr` | ⚠ |
| `ParserMatchClass` | `Operand`, `RegisterOperand` | — | `insn-directive-match-table` | — | ⚠ |
| `PrintMethod` | `Operand`, `RegisterOperand` | ✓ | `asm-writer` | — | ⚠ |
| `Type` | `Operand` | ✓ | `global-isel-combiner`, `x86-fold-tables`, `x86-instr-mapping` | — | — |
| `GIZeroRegister` | `RegisterOperand` | — | `global-isel` | — | — |
| `RegClass` | `RegisterOperand` | ✓ | `fast-isel`, `global-isel`, `instr-info` | `helper:DAGISelMatcherGen`, `helper:X86RecognizableInstr` | — |
| `Variant` | `AsmParserVariant` | — | `asm-writer`, `x86-fold-tables`, `x86-mnemonic-tables` | — | — |
| `EmitPriority` | `InstAlias` | — | `asm-writer` | — | — |
| `ResultInst` | `InstAlias` | ✓ | `asm-writer`, `pseudo-lowering` | — | — |
| `AssemblerCondDag` | `Predicate` | ✓ | `asm-writer` | — | — |
| `AssemblerMatcherPredicate` | `Predicate` | ✓ | `asm-writer`, `compress-inst-emitter` | `helper:DecoderEmitter` | — |
| `CondString` | `Predicate` | ✓ | `global-isel`, `global-isel-combiner`, `x86-instr-mapping` | — | — |

矩阵覆盖 54 个字段（共 166 个）。
<!-- END: GENERATED PROJECTION MATRIX -->

<!-- BEGIN: GENERATED DERIVATIONS -->

Record 里只有声明；这些是共享 C++ 在投影之前对语义的改写。

| 改写 | Schema 键 | 证据 | 说明 |
| --- | --- | --- | --- |
| `alias construction` | `InstAlias` | `llvm/utils/TableGen/Common/CodeGenInstAlias.cpp:134` | 把 `ResultInst` dag 展开成别名操作数，并解析寄存器/立即数。 |
| `operand class hierarchy` | `AsmOperandClass.SuperClasses` | `llvm/utils/TableGen/AsmMatcherEmitter.cpp:1520` | 把父类列表展开成 `isSubclass()` 判定的偏序关系。 |
| `register ordering` | `Register` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:1264` | 按 `PositionOrder`/名字排序，决定寄存器枚举与匹配表顺序。 |
| `operand type resolution` | `RegisterOperand.RegClass` | `llvm/utils/TableGen/AsmMatcherEmitter.cpp:435`, `llvm/utils/TableGen/AsmMatcherEmitter.cpp:1240` …(+3) | 把 `RegisterOperand` 解析到具体 `RegisterClass`。 |
| `mnemonic lowering` | `MnemonicAlias` | `llvm/utils/TableGen/AsmMatcherEmitter.cpp:3214` | 收集助记符别名，生成匹配前的重写表。 |
| `token substitution` | `TokenAlias` | `llvm/utils/TableGen/AsmMatcherEmitter.cpp:1798` | 把 FromToken 的匹配类标记为 ToToken 的子类。 |
| `subtarget predicate collection` | `Predicate.AssemblerMatcherPredicate` | `llvm/utils/TableGen/AsmMatcherEmitter.cpp:1632`, `llvm/utils/TableGen/Common/SubtargetFeatureInfo.cpp:25` | 收集汇编器谓词并分配 feature bit，供匹配表做条件检查。 |
<!-- END: GENERATED DERIVATIONS -->

<!-- BEGIN: GENERATED TD USAGE -->

| 文件:行 | 字段 | 契约状态 |
| --- | --- | --- |
| `llvm/lib/Target/RVMin/RVMin.td:128` | `AsmWriterClassName` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMin.td:135` | `ShouldEmitMatchRegisterAltName` | 消费（project） |
| `llvm/lib/Target/RVMin/RVMin.td:136` | `AllowDuplicateRegisterNames` | 消费（project） |
| `llvm/lib/Target/RVMin/RVMin.td:144` | `InstructionSet` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMin.td:145` | `AssemblyParsers` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMin.td:146` | `AssemblyWriters` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:12` | `FallbackRegAltNameIndex` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:17` | `Namespace` | 消费（derive+project） |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:18` | `HWEncoding` | 寄存器编码；`-gen-register-info` 消费。 |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:19` | `AltNames` | 消费（project） |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:20` | `DwarfNumbers` | DWARF 编号；`-gen-register-info` 消费。 |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:25` | `RegAltNameIndices` | 消费（project） |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:79` | `DecoderMethod` | decoder 自定义方法。 |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:107` | `Namespace` | 消费（derive+project） |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:108` | `HWEncoding` | 寄存器编码；`-gen-register-info` 消费。 |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:109` | `AltNames` | 消费（project） |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:110` | `DwarfNumbers` | DWARF 编号；`-gen-register-info` 消费。 |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:113` | `RegAltNameIndices` | 消费（project） |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:168` | `DecoderMethod` | decoder 自定义方法。 |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:179` | `DecoderMethod` | decoder 自定义方法。 |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:186` | `DecoderMethod` | decoder 自定义方法。 |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:191` | `DecoderMethod` | decoder 自定义方法。 |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:16` | `Num` | 自定义字段（需自行保证 name-based 读取方存在） |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:106` | `Num` | 自定义字段（需自行保证 name-based 读取方存在） |

共 24 处使用；契约外 4 处。
<!-- END: GENERATED TD USAGE -->

RVMin 的 `.td` 使用只有 13 处，其中 4 处不在本后端契约里：

- `AsmWriterClassName` / `AssemblyWriters`：属于 asm-writer；
- `FallbackRegAltNameIndex`：asm-writer 消费，asm matcher 的回退由
  `ShouldEmitMatchRegisterAltName` 控制；
- `InstructionSet`：见“常见坑”第 3 条。

## 构建与验证

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop
T="$W/llvm/utils/TableGen/dsl-surface.py"
DOC="$W/llvm/lib/Target/RVMin/docs/34-asm-matcher-dsl.md"

# 1. 直接看契约
python3 "$T" --backend asm-matcher
python3 "$T" --backend asm-matcher --matrix

# 2. 文档块更新/校验
python3 "$T" --backend asm-matcher --update "$DOC" \
  --usage "$W/llvm/lib/Target/RVMin/RVMin.td" \
          "$W/llvm/lib/Target/RVMin/RVMinRegisterInfo.td"
python3 "$T" --backend asm-matcher --check "$DOC" \
  --usage "$W/llvm/lib/Target/RVMin/RVMin.td" \
          "$W/llvm/lib/Target/RVMin/RVMinRegisterInfo.td"

# 3. 跨后端矩阵与字段影响面
python3 "$T" --status-matrix --usage \
  "$W/llvm/lib/Target/RVMin/RVMinRegisterInfo.td" \
  "$W/llvm/lib/Target/RVMin/RVMin.td"
python3 "$T" --effect ParserMatchClass

# 4. 全量门禁
"$W/llvm/lib/Target/RVMin/scripts/rvmin-verify-all.sh" "$B/bin"
```

## 预期输出

```text
$ python3 dsl-surface.py --backend asm-matcher
asm-matcher: {'roots': 15, 'fields': 166, 'consumed': 142,
              'unused': 24, 'stages': {'schema': 24, 'derive': 95,
              'project': 42, 'derive+project': 5}}

$ python3 dsl-surface.py --backend asm-matcher --check doc34.md --usage ...
dsl-surface: OK (asm-matcher: 142/166 fields consumed
              [95 derive, 42 project, 5 both], 2 magic names, 7 derivations)

$ python3 dsl-surface.py --status-matrix --usage ...
dsl-surface: OK (cross-backend status: 13 usages, 4 backends)

$ scripts/rvmin-verify-all.sh "$B/bin"
...
PASS 7/7: TableGen DSL contracts + status matrix + field effect
ALL GATES PASS (run rvmin-run.sh separately for the QEMU check)
```

## 常见坑

1. **`sources` 是硬边界。** 没列进去的读取点不会出现在契约里。`PositionOrder`
   在 `Record.h` 里被排序器读取，所以本后端只能标成“schema 未消费”。
2. **`roots_from` 决定“枚举哪些类”，不决定“读哪些字段”。** 根类上的字段
   可以完全不被本后端消费（24 个 `schema` 字段就是这种情况）。
3. **`Target` 不是根，字段就不会进 schema。** `InstructionSet` 被
   `CodeGenTarget` 读取，但 asm matcher 不调用 `getTargetInstructionSet()`；
   它也因此不会出现在契约里，`--usage` 会把它标成“契约外”。
4. **`project` 多不等于更复杂。** `AsmOperandClass` 的 9 个字段全是 `project`，
   因为只有 emitter 读它们；`derive` 的 95 个大多来自共享寄存器模型。
5. **同名多声明要按类读。** `Name` 同时属于 `AsmOperandClass` 和
   `AsmParserVariant`；`DiagnosticType`/`DiagnosticString` 同时属于
   `RegisterClass` 和 `AsmOperandClass`。注解是按字段名合并的，读报告时看
   “声明于”列。
6. **加入第四后端会改动第 32/33 章的生成块。** 那是 SSOT 在生效，不是文档
   漂移；重新 `--update` 即可。
7. **`--usage` 的“契约外”不都是错误。** 目标文件可能同时服务多个后端；
   `AsmWriterClassName` 对 asm-writer 是核心输入，对 asm matcher 就是无关字段。

## 练习

1. 把 `CodeGenTarget.cpp` 加回 `roots_from`，观察 `FusionPredicates` 如何
   污染契约，再用 `extra_roots` + `ambient_fields` 恢复。
2. 给 `AsmParser` 增加一个字段并在 emitter 里读它，确认 `--check` 报
   “missing annotations”而不是静默通过。
3. 用 `--effect ParserMatchClass` 看当时四个后端的投影，解释为什么只有
   asm-matcher 是 `project`。
4. 用 `--effect SuperClasses` 对比 `AltNames`：一个是纯 emitter 字段，
   一个是跨后端字段。
5. 给 `--status-matrix` 增加“只显示某个后端非 `—` 的行”的过滤选项，
   需要哪些数据？会不会破坏现有生成块？

## 产物清单

| 文件 | 变更 |
| --- | --- |
| `llvm/utils/TableGen/dsl-surface.py` | 新增 `asm-matcher` Backend；66 个字段注解；7 条 derivation；2 个 magic name |
| `llvm/lib/Target/RVMin/docs/34-asm-matcher-dsl.md` | 新增：本章（四个生成块） |
| `llvm/lib/Target/RVMin/docs/32-cross-backend-status.md` | 生成块升级为四后端 |
| `llvm/lib/Target/RVMin/docs/33-field-effect.md` | 生成块升级为四后端 |
| `llvm/lib/Target/RVMin/docs/README.md` | 新增第 34 章索引行；范围改为 29–34 |
| `llvm/lib/Target/RVMin/scripts/rvmin-verify-all.sh` | 第 7 项增加 asm-matcher 校验 |
