# 第 30 章：第二个后端——把 DSL 契约推广到 `-gen-instr-info`

## 目标

第 29 章把 `-gen-register-info` 的 DSL 语义从 emitter 源码里反推出来。本章
不写新指令、不改代码生成，只回答一个问题：

> 那套契约提取器是只为寄存器后端定制的，还是能复用到别的 `-gen-*`？

实验对象是 `-gen-instr-info`。它和 `-gen-register-info` 共享同一套
`.td` → `Record` → 派生模型 → 投影的流水线，但 schema 大得多，也暴露出
第 29 章没遇到的两类问题：

1. **无 body 的继承声明**：`class TIIPredicate<...> : FunctionPredicateBase<...>;`
   没有大括号，字段全在基类里；
2. **前向声明遮蔽真实定义**：`TargetInstrPredicate.td` 里有一句
   `class Instruction;`，解析器不能让它在 `Target.td` 的真实定义之后覆盖它。

本章的产出：

- `dsl-surface.py` 增加 `--backend instr-info`，所有后端相关表按后端配置；
- 一份 `instr-info` 契约：6 个记录类入口、111 个字段、85 个被消费、
  40 行跨后端投影、8 条派生改写；
- 证明第 29 章的 `(declaring class, field)` 主键和四层模型可以原样复用。

非目标：

- 不修改 `InstrInfoEmitter` 的行为；
- 不把 111 个字段逐个做成教程正文；契约块负责穷举，正文只讲新增规则；
- 不覆盖 `-gen-asm-writer` / `-gen-asm-matcher`。

## 背景：为什么第二个后端不是复制粘贴

先看规模对比：

| 指标 | `register-info` | `instr-info` |
| --- | --- | --- |
| 记录类入口 | 8 | 6 |
| 声明字段 | 59 | 111 |
| 被消费字段 | 44 | 85 |
| 未消费字段 | 15 | 26 |
| emitter 自己读的字段 | 5 | 11 |
| 跨后端矩阵行 | 21 | 40 |
| 派生改写 | 11 | 8 |
| 硬编码记录名 | 10 | 0 |

两个后端都符合第 29 章的核心结论：**emitter 自己的字段读取只是冰山一角**。
`instr-info` 的 85 个被消费字段里，74 个停在 `derive` 层（只被
`CodeGenInstruction` / `CodeGenTarget` / `CodeGenRegisters` 等共享代码读），
只有 5 个是 emitter 独占的 `project` 字段，6 个是 `derive+project`。

但 `instr-info` 的 schema 形状不同：

- `Instruction` 是“一长串 MCID 布尔标志 + 操作数 dag + 编码标志”；
- `Operand` / `RegisterOperand` 描述 MC operand 的类型与打印/编码方法；
- `TIIPredicate` 用 `code` 字段直接注入目标 C++；
- 大量字段其实是别的后端（asm writer / asm matcher / DAG isel）的输入，
  在 `instr-info` 里被正确标成“声明了但不读”。

所以第二个后端不能靠复制 `register-info` 的配置，必须把
`Backend` 描述、注解、名字表、派生表、主产物全部**按后端参数化**。

## 代码地图

| 文件 | 作用 |
| --- | --- |
| `llvm/utils/TableGen/InstrInfoEmitter.cpp` | 本后端 emitter；1485 行 |
| `llvm/utils/TableGen/Common/CodeGenInstruction.{h,cpp}` | 把 `Instruction` 的 dag/字段变成 `CodeGenInstruction` |
| `llvm/utils/TableGen/Common/CodeGenTarget.{h,cpp}` | `getInstructions()`、`getInstructionSet()`、寄存器类模型 |
| `llvm/utils/TableGen/Common/CodeGenRegisters.{h,cpp}` | 操作数用到的寄存器类/子寄存器模型 |
| `llvm/include/llvm/Target/Target.td` | `Instruction` / `Operand` / `RegisterOperand` 等 schema |
| `llvm/include/llvm/Target/TargetInstrPredicate.td` | `TIIPredicate` / `MCStatement` |
| `llvm/utils/TableGen/dsl-surface.py` | **本章改造**：按后端配置的契约提取器 |
| `llvm/lib/Target/RVMin/docs/30-instr-info-dsl.md` | 本章；四个生成块由脚本生成 |

## 关键实现：把 Backend 配置化

### 一个 `Backend` 条目描述一个投影

第 29 章的 `Backend` 只描述“扫哪些文件、从哪找根”。本章把它扩展成完整的
后端配置：

```python
"instr-info": Backend(
    name="instr-info",
    emitter="llvm/utils/TableGen/InstrInfoEmitter.cpp",
    sources=(emitter, CodeGenInstruction, CodeGenTarget,
             CodeGenRegisters, CodeGenHwModes, InfoByHwMode),
    td_files=("Target.td", "TargetInstrPredicate.td"),
    roots_from=("InstrInfoEmitter.cpp",),
    extra_roots=(("Instruction", "CodeGenTarget.cpp",
                  'getAllDerivedDefinitions("Instruction")'),),
    helper_classes=("HwModeSelect", "RegInfoByHwMode", "RegInfo",
                    "SubRegRangeByHwMode", "SubRegRange", "DwarfRegAlias",
                    "RegClassByHwMode"),
)
```

配套的五张按后端表：

| 表 | 作用 |
| --- | --- |
| `ANNOTATIONS[backend]` | 每个被消费字段的人类语义；缺失即 `--check` 失败 |
| `UNUSED_NOTES[backend]` | 声明了但本后端不读的字段归属 |
| `MAGIC_NAMES[backend]` | 硬编码记录名；`instr-info` 为空元组 |
| `DERIVATIONS[backend]` | 投影前的共享改写；每条正则必须命中 |
| `MAIN_OUTPUTS[backend]` | 渲染头里的主产物名 |

`Instruction` 为什么放在 `extra_roots` 而不是 `roots_from`？因为它是在
`CodeGenTarget.cpp` 的 `getInstructions()` 里通过
`getAllDerivedDefinitions("Instruction")` 枚举的。把整个
`CodeGenTarget.cpp` 加进 `roots_from` 会顺带拉进 `Target`、`AsmParser` 等
无关根——这正是第 29 章“常见坑 2”的同类问题。

### 两个 `.td` 解析修正

`TargetInstrPredicate.td` 暴露了第 29 章解析器的两个局限：

1. **无 body 继承声明**：`class TIIPredicate<...> : FunctionPredicateBase<...>;`
   没有 `{}`。旧解析器直接跳过，导致 `TIIPredicate` 的
   `FunctionName` / `Body` 字段完全消失。现在会记录继承列表，字段从基类继承。
2. **前向声明不能覆盖真实定义**：`TargetInstrPredicate.td` 里有
   `class Instruction;`。解析顺序是 `Target.td` 在前，如果允许空声明覆盖，
   `Instruction` 会退化成 0 字段。现在“无 body 且无基类”的声明只在不与已有
   定义冲突时登记。

另外，基类列表的逗号必须尊重模板嵌套：`FunctionPredicateBase<Name, body>`
里的逗号不是基类分隔符。`_parse_bases()` 现在按 `<>` / `()` 深度切分。

### 机器契约

下面这一段由脚本生成，不要手改；改 emitter 后运行 `--update`。

<!-- BEGIN: GENERATED DSL CONTRACT -->

后端：`gen-instr-info` · emitter：`llvm/utils/TableGen/InstrInfoEmitter.cpp`
CLI：`-instr-info-expand-mi-operand-info`
统计：6 个记录类入口 · 111 个字段（schema 26 · derive 74 · project 5 · derive+project 6）。

**记录类入口**

| 记录类 | 定义 | 消费证据 |
| --- | --- | --- |
| `RegisterClass` | `llvm/include/llvm/Target/Target.td:292` | `llvm/utils/TableGen/InstrInfoEmitter.cpp:460` |
| `Instruction` | `llvm/include/llvm/Target/Target.td:608` | `llvm/utils/TableGen/Common/CodeGenTarget.cpp:287` |
| `Operand` | `llvm/include/llvm/Target/Target.td:1001` | `llvm/utils/TableGen/InstrInfoEmitter.cpp:456`, `llvm/utils/TableGen/InstrInfoEmitter.cpp:1195` |
| `RegisterOperand` | `llvm/include/llvm/Target/Target.td:1051` | `llvm/utils/TableGen/InstrInfoEmitter.cpp:458` |
| `TargetSpecializedStandardPseudoInstruction` | `llvm/include/llvm/Target/Target.td:1614` | `llvm/utils/TableGen/InstrInfoEmitter.cpp:898` |
| `TIIPredicate` | `llvm/include/llvm/Target/TargetInstrPredicate.td:311` | `llvm/utils/TableGen/InstrInfoEmitter.cpp:664`, `llvm/utils/TableGen/InstrInfoEmitter.cpp:872` |

**字段契约**（主键是 `(declaring class, field)`；`stage` 表示它停在哪一层，`⚠` 表示同名多声明）

| 字段 | 声明于 | 类型 | 默认值 | stage | 本后端读取点 | 语义 |
| --- | --- | --- | --- | --- | --- | --- |
| `Modes` | `HwModeSelect` | `list<HwMode>` | `Ms` | `derive` | `llvm/utils/TableGen/Common/CodeGenHwModes.cpp:40` | HwMode 列表；与 `Objects` 等长，按 ModeId 绑定。 |
| `RegSize` | `RegInfo` | `int` | `RS` | `derive` | `llvm/utils/TableGen/Common/InfoByHwMode.cpp:105` | `RegInfo` 的寄存器位数。 |
| `SpillAlignment` | `RegInfo` | `int` | `SA` | `derive` | `llvm/utils/TableGen/Common/InfoByHwMode.cpp:107` | `RegInfo` 的 spill slot 对齐。 |
| `SpillSize` | `RegInfo` | `int` | `SS` | `derive` | `llvm/utils/TableGen/Common/InfoByHwMode.cpp:106` | `RegInfo` 的 spill slot 位数。 |
| `Objects` | `RegInfoByHwMode`, `SubRegRangeByHwMode`, `RegClassByHwMode` | `list<RegInfo>` / `list<SubRegRange>` / `list<RegisterClass>` | `Ts` / `RegClasses` | `derive+project` ⚠ | `llvm/utils/TableGen/InstrInfoEmitter.cpp:159`, `llvm/utils/TableGen/Common/CodeGenHwModes.cpp:41`, `llvm/utils/TableGen/Common/CodeGenTarget.cpp:119` | 按 HwMode 绑定的对象列表；`RegisterByHwMode` 是寄存器，`RegInfoByHwMode`/`SubRegRangeByHwMode` 是参数。 |
| `Offset` | `SubRegRange` | `int` | `offset` | `derive` | `llvm/utils/TableGen/Common/InfoByHwMode.cpp:189` | 子寄存器/范围的起始位；`-1` 表示不固定。 |
| `Size` | `SubRegRange`, `RegisterClass`, `Instruction` | `int` | `size` / `0` | `derive+project` ⚠ | `llvm/utils/TableGen/InstrInfoEmitter.cpp:566`, `llvm/utils/TableGen/InstrInfoEmitter.cpp:1312`, `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:800`, `llvm/utils/TableGen/Common/InfoByHwMode.cpp:188` | 指令编码字节数；`emitRecord` 写入 MCInstrDesc 的 Size。同名 `RegisterClass.Size` 是 spill size。 |
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
| `Namespace` | `RegisterClass`, `Instruction` | `string` | `namespace` / `""` | `derive` ⚠ | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:419`, `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:58`, `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:59`, `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:796` …(+3) | 指令/寄存器命名空间；`Target.getInstNamespace()` 决定生成 enum 的 C++ namespace。 |
| `RegInfos` | `RegisterClass` | `RegInfoByHwMode` | `（未设默认值）` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:798` | 按 HwMode 的 `{RegSize,SpillSize,SpillAlignment}` 三元组；覆盖默认模式值。 |
| `RegTypes` | `RegisterClass` | `list<ValueType>` | `regTypes` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:759` | 寄存器类支持的 MVT 列表；`Size==0` 时用于推断位宽。 |
| `SpillStackID` | `RegisterClass` | `int` | `0` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:814` | spill stack slot 的自定义 ID；影响 frame index 分配。 |
| `TSFlags` | `RegisterClass`, `Instruction` | `bits<8>` / `bits<64>` | `0` | `derive+project` ⚠ | `llvm/utils/TableGen/InstrInfoEmitter.cpp:1415`, `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:825` | 目标自定义标志位；`emitRecord` 校验位宽并写入 MCInstrDesc 的 TSFlags。同名 `RegisterClass.TSFlags` 是寄存器类标志。 |
| `Weight` | `RegisterClass` | `int` | `?` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:920` | 寄存器类的压力权重；影响压力集计算。 |
| `isAllocatable` | `RegisterClass` | `bit` | `true` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:812` | 是否可分配；决定 `InAllocatableClassTable`。 |
| `AsmString` | `Instruction` | `string` | `""` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:420` | 汇编模板；`CodeGenInstruction` 保存，供 asm writer/matcher 使用；本后端只解析不直接发射。 |
| `Constraints` | `Instruction` | `string` | `""` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:480` | 内联汇编约束字符串；`ParseConstraints` 为每个操作数填约束位。 |
| `Defs` | `Instruction` | `list<Register>` | `[]` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:472` | 隐式定义寄存器列表；合并进 `ImplicitDefs`，发射到 `ImplicitLists`。 |
| `FastISelShouldIgnore` | `Instruction` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:453` | FastISel 忽略该指令；只保存在 `CodeGenInstruction`，不进入 MCInstrDesc。 |
| `InOperandList` | `Instruction` | `dag` | `（未设默认值）` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:43` | 输入操作数 dag；展开成 use 列表，参与操作数编号与 `MIOperandInfo` 解析。 |
| `OutOperandList` | `Instruction` | `dag` | `（未设默认值）` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:29` | 输出操作数 dag；`CodeGenInstruction` 展开成 def 列表，决定 `NumDefs`。 |
| `Predicates` | `Instruction` | `list<Predicate>` | `[]` | `derive+project` | `llvm/utils/TableGen/InstrInfoEmitter.cpp:747`, `llvm/utils/TableGen/InstrInfoEmitter.cpp:800`, `llvm/utils/TableGen/Common/CodeGenHwModes.cpp:27` | 指令谓词列表；`computeRequiredFeatures` 用它编码每个 opcode 的 feature bitset。 |
| `UseLogicalOperandMappings` | `Instruction` | `bit` | `false` | `project` | `llvm/utils/TableGen/InstrInfoEmitter.cpp:591`, `llvm/utils/TableGen/InstrInfoEmitter.cpp:958` | 为 true 时发射 `getLogicalOperandSize/Idx` 表。 |
| `UseNamedOperandTable` | `Instruction` | `bit` | `false` | `project` | `llvm/utils/TableGen/InstrInfoEmitter.cpp:388`, `llvm/utils/TableGen/InstrInfoEmitter.cpp:960` | 为 true 时发射 `getNamedOperandIdx` 等命名操作数表。 |
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
| `EncoderMethod` | `Operand`, `RegisterOperand` | `string` | `""` | `derive` ⚠ | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:90`, `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:96`, `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:181` | 自定义编码函数名；`CodeGenInstruction` 保存，供 encoder 调用。 |
| `MIOperandInfo` | `Operand` | `dag` | `(ops)` | `derive+project` | `llvm/utils/TableGen/InstrInfoEmitter.cpp:1198`, `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:97` | 复合操作数的子操作数 dag；`CollectOperandInfo` 用它展开 MC operand 列表。 |
| `OperandType` | `Operand`, `RegisterOperand` | `string` | `"OPERAND_UNKNOWN"` / `"OPERAND_REGISTER"` | `derive` ⚠ | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:88`, `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:93` | MC operand 的 C++ 类型名；`CodeGenInstruction` 存入 `OperandType`。 |
| `PrintMethod` | `Operand`, `RegisterOperand` | `string` | `"printOperand"` / `pm` | `derive` ⚠ | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:87`, `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:92` | 自定义打印函数名；`CodeGenInstruction` 保存，供 asm writer 调用。 |
| `RegClass` | `RegisterOperand` | `RegisterClassLike` | `regclass` | `derive+project` | `llvm/utils/TableGen/InstrInfoEmitter.cpp:156`, `llvm/utils/TableGen/Common/CodeGenTarget.cpp:274`, `llvm/utils/TableGen/Common/InfoByHwMode.cpp:224` | 操作数绑定的寄存器类；解析为 `RegisterClass` 并用于寄存器操作数信息。 |
| `Instruction` | `TargetSpecializedStandardPseudoInstruction` | `StandardPseudoInstruction` | `base_inst` | `project` | `llvm/utils/TableGen/InstrInfoEmitter.cpp:905` | `TargetSpecializedStandardPseudoInstruction` 的基指令；用于建立特化伪指令到标准伪指令的映射。 |
| `Body` | `FunctionPredicateBase` | `MCStatement` | `body` | `project` | `llvm/utils/TableGen/InstrInfoEmitter.cpp:704`, `llvm/utils/TableGen/InstrInfoEmitter.cpp:892` | `TIIPredicate` 的 `MCStatement` 函数体；`expandStatement` 直接展开成目标 C++ 代码。 |
| `FunctionName` | `FunctionPredicateBase` | `string` | `name` | `project` | `llvm/utils/TableGen/InstrInfoEmitter.cpp:684`, `llvm/utils/TableGen/InstrInfoEmitter.cpp:700`, `llvm/utils/TableGen/InstrInfoEmitter.cpp:883` | `TIIPredicate` 的 C++ 函数名；`PredicateExpander` 用它生成 `InstrInfo` 方法。 |

`stage`：`schema` 仅声明 · `derive` 只被共享派生层读 · `project` 只被本 emitter 读 · `derive+project` 两者都读。

**声明了但本后端不读的字段**

| 字段 | 声明于 | 类型 | 归属 |
| --- | --- | --- | --- |
| `DecoderMethod` | `DAGOperand`, `InstructionEncoding` | `string` | decoder 自定义方法。 |
| `hasCompleteDecoder` | `DAGOperand`, `InstructionEncoding` | `bit` | decoder 完整性标记。 |
| `CoerceFromRegClass` | `RegisterClass` | `RegisterClass` | asm matcher 的寄存器类强制。 |
| `DiagnosticString` | `RegisterClass` | `string` | asm matcher 诊断文本。 |
| `DiagnosticType` | `RegisterClass` | `string` | asm matcher 诊断类型。 |
| `altNameIndex` | `RegisterClass` | `RegAltNameIndex` | 寄存器类别名索引；asm writer 消费。 |
| `DwarfAlias` | `DwarfRegAlias` | `Register` | DWARF 别名；`-gen-register-info` 消费。 |
| `DecoderNamespace` | `InstructionEncoding` | `string` | decoder 生成 namespace。 |
| `AddedComplexity` | `Instruction` | `int` | isel pattern 优先级；`-gen-dag-isel` 消费。 |
| `AsmMatchConverter` | `Instruction` | `string` | asm matcher 的自定义转换器。 |
| `AsmVariantName` | `Instruction` | `string` | asm variant 选择。 |
| `CodeSize` | `Instruction` | `int` | isel 的 code size 排序；`-gen-dag-isel` 消费。 |
| `EncodingInfos` | `Instruction` | `EncodingByHwMode` | 按 HwMode 的编码；encoder/decoder 消费。 |
| `GISelMatchGenericTypes` | `Instruction` | `bit` | GlobalISel 泛型类型匹配。 |
| `HasPositionOrder` | `Instruction` | `bit` | asm matcher 的位置排序。 |
| `Itinerary` | `Instruction` | `InstrItinClass` | 旧调度模型；`-gen-instr-info` 不读。 |
| `Pattern` | `Instruction` | `list<dag>` | isel pattern；`-gen-dag-isel` 消费。 |
| `PostEncoderMethod` | `Instruction` | `string` | 编码后处理函数。 |
| `SchedRW` | `Instruction` | `list<SchedReadWrite>` | 新调度模型读写资源。 |
| `TwoOperandAliasConstraint` | `Instruction` | `string` | 两操作数 alias 约束；asm matcher 消费。 |
| `isAsmParserOnly` | `Instruction` | `bit` | 仅汇编器使用，codegen 不可见。 |
| `supportsDeactivationSymbol` | `Instruction` | `bit` | 指令级 deactivation 标记；其它后端消费。 |
| `MCOperandPredicate` | `Operand` | `code` | MC operand 谓词；asm writer 消费。 |
| `ParserMatchClass` | `Operand`, `RegisterOperand` | `AsmOperandClass` | asm parser 匹配类。 |
| `Type` | `Operand` | `ValueType` | 操作数 ValueType；isel/asm 消费。 |
| `GIZeroRegister` | `RegisterOperand` | `Register` | GlobalISel 零寄存器。 |

本后端没有硬编码记录名（名字只作为数据/生成标识符使用）。

<!-- END: GENERATED DSL CONTRACT -->

### 跨后端投影矩阵

`instr-info` 的字段大量被 asm writer / asm matcher / DAG isel 共享。矩阵现在
第一次出现 `register-info` 作为“其它后端”的行（`Namespace`、`DwarfAlias` 等），
验证了矩阵是跨后端视角，而不是单后端清单。

<!-- BEGIN: GENERATED PROJECTION MATRIX -->

只列“本后端之外还有读者”或“同名多声明”的字段；`shared` 表示共享派生层也读它。

| 字段 | 声明于 | shared | 其它 `-gen-*` | helper | 同名 |
| --- | --- | --- | --- | --- | --- |
| `Objects` | `RegInfoByHwMode`, `SubRegRangeByHwMode`, `RegClassByHwMode` | ✓ | — | — | ⚠ |
| `Size` | `SubRegRange`, `RegisterClass`, `Instruction` | ✓ | `asm-matcher`, `callingconv`, `compress-inst-emitter` | `helper:DecoderEmitter`, `helper:X86RecognizableInstr` | ⚠ |
| `DecoderMethod` | `DAGOperand`, `InstructionEncoding` | ✓ | — | — | ⚠ |
| `hasCompleteDecoder` | `DAGOperand`, `InstructionEncoding` | ✓ | — | — | ⚠ |
| `Alignment` | `RegisterClass` | ✓ | — | `helper:X86RecognizableInstr` | — |
| `CoerceFromRegClass` | `RegisterClass` | — | `asm-matcher` | — | — |
| `DiagnosticString` | `RegisterClass` | — | `asm-matcher` | — | — |
| `DiagnosticType` | `RegisterClass` | — | `asm-matcher` | — | — |
| `Namespace` | `RegisterClass`, `Instruction` | ✓ | `asm-matcher`, `asm-writer`, `emitter`, `fast-isel`, `pseudo-lowering`, `register-info`, `subtarget` | — | ⚠ |
| `TSFlags` | `RegisterClass`, `Instruction` | ✓ | — | — | ⚠ |
| `DwarfAlias` | `DwarfRegAlias` | — | `register-info` | — | — |
| `DecoderNamespace` | `InstructionEncoding` | ✓ | — | `helper:DecoderEmitter` | — |
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
| `Predicates` | `Instruction` | ✓ | `asm-matcher`, `asm-writer`, `compress-inst-emitter`, `global-isel-combiner`, `instr-docs`, `macro-fusion-pred`, `subtarget`, `x86-instr-mapping` | `helper:DecoderEmitter`, `helper:X86RecognizableInstr` | — |
| `TwoOperandAliasConstraint` | `Instruction` | — | `asm-matcher` | — | — |
| `Uses` | `Instruction` | ✓ | `x86-fold-tables` | — | — |
| `isAsmParserOnly` | `Instruction` | — | `x86-fold-tables`, `x86-instr-mapping` | `helper:DecoderEmitter`, `helper:X86RecognizableInstr` | — |
| `isCodeGenOnly` | `Instruction` | ✓ | — | `helper:DecoderEmitter`, `helper:X86RecognizableInstr` | — |
| `isPseudo` | `Instruction` | ✓ | `searchable-tables`, `x86-fold-tables` | — | — |
| `supportsDeactivationSymbol` | `Instruction` | — | — | `helper:DAGISelMatcherEmitter` | — |
| `EncoderMethod` | `Operand`, `RegisterOperand` | ✓ | — | — | ⚠ |
| `MCOperandPredicate` | `Operand` | — | `asm-writer` | — | — |
| `MIOperandInfo` | `Operand` | ✓ | — | `helper:DAGISelMatcherGen` | — |
| `OperandType` | `Operand`, `RegisterOperand` | ✓ | `asm-writer`, `global-isel-combiner` | `helper:X86RecognizableInstr` | ⚠ |
| `ParserMatchClass` | `Operand`, `RegisterOperand` | — | `asm-matcher`, `insn-directive-match-table` | — | ⚠ |
| `PrintMethod` | `Operand`, `RegisterOperand` | ✓ | `asm-writer` | — | ⚠ |
| `Type` | `Operand` | ✓ | `global-isel-combiner`, `x86-fold-tables`, `x86-instr-mapping` | — | — |
| `GIZeroRegister` | `RegisterOperand` | — | `global-isel` | — | — |
| `RegClass` | `RegisterOperand` | ✓ | `asm-matcher`, `fast-isel`, `global-isel` | `helper:DAGISelMatcherGen`, `helper:X86RecognizableInstr` | — |

矩阵覆盖 40 个字段（共 111 个）。
<!-- END: GENERATED PROJECTION MATRIX -->

### 派生层改写（Record → 投影之间的模型变换）

`instr-info` 的派生层重点是“dag → 操作数列表”和“约束字符串 → 约束位”。
`TargetSpecializedStandardPseudoInstruction` 还会从基指令复制字段，属于典型的
“Record 里没有、投影时才出现”的语义。

<!-- BEGIN: GENERATED DERIVATIONS -->

Record 里只有声明；这些是共享 C++ 在投影之前对语义的改写。

| 改写 | Schema 键 | 证据 | 说明 |
| --- | --- | --- | --- |
| `operand list expansion` | `Instruction.OutOperandList/InOperandList` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:24` | 把 dag 展开成 OperandInfo；同时解析 `MIOperandInfo` 复合操作数。 |
| `constraint parsing` | `Instruction.Constraints` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:394`, `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:480` | 把约束字符串拆成每个操作数的约束位。 |
| `instruction ordering` | `Instruction` | `llvm/utils/TableGen/Common/CodeGenTarget.cpp:287` | 按记录名排序，决定 opcode 编号与 `NumberedInstructions` 顺序。 |
| `pseudo specialization` | `TargetSpecializedStandardPseudoInstruction` | `llvm/utils/TableGen/InstrInfoEmitter.cpp:80`, `llvm/utils/TableGen/InstrInfoEmitter.cpp:897` …(+1) | 建立特化伪指令到标准伪指令的映射，并继承其操作数/标志。 |
| `hw-mode selection` | `HwModeSelect.Objects` | `llvm/utils/TableGen/Common/CodeGenHwModes.cpp:42` | 按 ModeId 把 `Objects[i]` 绑定到 `Modes[i]`。 |
| `inference` | `RegisterClass` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:1369`, `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:2795` …(+1) | 从显式寄存器类推断出缺失的类；只存在于派生模型。 |
| `parameterization` | `RegInfo` | `llvm/utils/TableGen/Common/InfoByHwMode.cpp:105` | 把 `RegInfo` 三元组转成内部 `RegSizeInfo`。 |
| `operand type resolution` | `RegisterOperand.RegClass` | `llvm/utils/TableGen/InstrInfoEmitter.cpp:156`, `llvm/utils/TableGen/Common/CodeGenTarget.cpp:274` …(+1) | 把 `RegisterOperand` 解析到具体 `RegisterClass`，决定寄存器操作数信息。 |
<!-- END: GENERATED DERIVATIONS -->

### RVMin 的真实用法对照

RVMin 的顶层 `RVMin.td` 只写了 6 个 target 配置字段，它们全部不属于
`instr-info` 契约——这本身就是结论：`-gen-instr-info` 只关心指令 schema，
不关心汇编器/子目标配置。

<!-- BEGIN: GENERATED TD USAGE -->

| 文件:行 | 字段 | 契约状态 |
| --- | --- | --- |
| `llvm/lib/Target/RVMin/RVMin.td:163` | `AsmWriterClassName` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMin.td:170` | `ShouldEmitMatchRegisterAltName` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMin.td:171` | `AllowDuplicateRegisterNames` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMin.td:179` | `InstructionSet` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMin.td:180` | `AssemblyParsers` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMin.td:181` | `AssemblyWriters` | 不在本后端契约（其它后端消费或拼写错误） |

共 6 处使用；契约外 6 处。
<!-- END: GENERATED TD USAGE -->

指令文件 `RVMinInstrInfo.td` 的用法更多，手工归纳几条代表：

| `.td` 写法 | 契约状态 | 说明 |
| --- | --- | --- |
| `let PrintMethod = "printOperand"` | `derive` | 进入 `CodeGenInstruction::PrintMethod` |
| `let hasSideEffects = 0` | `derive` | 进入 `CodeGenInstruction`，最终影响 MCID 标志 |
| `let isTerminator = 1` / `isBranch = 1` | `derive` | MCID 标志 |
| `let Predicates = [HasM]` | `derive+project` | 共享层解析，emitter 还生成 feature bitset |
| `let TSFlags{4-0} = FmtI.Value` | `derive+project` | emitter 校验位宽并写入 MCInstrDesc |
| `let ParserMatchClass = ...` | 不在契约 | `-gen-asm-matcher` 的输入 |
| `let DecoderMethod = ...` | 不在契约 | `-gen-disassembler` 的输入 |
| `let Inst{31-25} = funct7` | 自定义字段 | RVMin 自己的 encoder 字段，不在通用 schema |

最后一行是本章最实用的结论：`--usage` 把 `Inst` 标成“契约外”不是错误，
而是提醒你它是**目标自定义字段**，只有读它的 C++ 才需要保证语义。

## 深读：`instr-info` 暴露出的新规则

### 1. `TIIPredicate` 是“无 body 的继承类”

```tablegen
class TIIPredicate<string Name, MCStatement body>
    : FunctionPredicateBase<Name, body>, MCInstPredicate;
```

它自己没有字段，`FunctionName` / `Body` 都在 `FunctionPredicateBase` 里。
契约表把它们记在 `FunctionPredicateBase` 名下，而不是 `TIIPredicate` 名下——
这正是 `(declaring class, field)` 主键的价值。

### 2. `Instruction` 的前向声明不能覆盖真实定义

`TargetInstrPredicate.td` 为了声明 `MCInstPredicate` 相关接口，写了一句
`class Instruction;`。如果解析器把无 body 声明当成完整定义，`Instruction`
会从 100+ 字段退化成 0 字段，整个契约瞬间失真。这类“同名空声明”在
TableGen 里很常见，解析器必须区分前向声明和真正的继承别名。

### 3. `UseNamedOperandTable` / `UseLogicalOperandMappings` 是整表开关

这两个字段只被 emitter 读（`project`），但一个 bit 就决定是否生成
`getNamedOperandIdx` / `getLogicalOperandSize` 等一整组函数。它们不属于
“可选的细节”，而是投影级的开关。

### 4. `Predicates` 同时停在两层

`CodeGenInstruction` / `CodeGenHwModes` 会读 `Predicates`（`derive`），
emitter 还会用它生成 `computeRequiredFeatures()` 的 feature bitset
（`project`），所以 stage 是 `derive+project`。同一字段在不同层有不同用途，
单看 emitter 或单看共享层都会低估它。

### 5. `TSFlags` 有位宽检查

emitter 在 `emitRecord` 里逐位检查 `TSFlags`，越界直接
`PrintFatalError(..., "Invalid TSFlags bit ...")`。所以 `TSFlags` 不是
“随便塞一个整数”，它的可用位宽由目标自己声明。同名 `RegisterClass.TSFlags`
也走共享层，但语义是寄存器类标志。

### 6. `OperandType` 枚举只为 X86 发射

`emitOperandTypeMappings()` 开头有：

```cpp
if (Namespace != "X86")
  return;
```

这是目标特定的投影策略：schema 对所有目标一致，但产物只对 X86 生成。
契约表只能告诉你字段被读，不能告诉你“只对某个 namespace 生效”——这类
条件需要放在深读里。

### 7. `Size` 再次发生语义碰撞

`Size` 同时是：

- `Instruction.Size`：指令编码字节数，写入 MCInstrDesc；
- `RegisterClass.Size`：spill size，`0` 表示按 `RegTypes` 推断；
- `SubRegRange.Size`：子寄存器/范围位宽。

契约表把它们合并成一行并标 `⚠`，语义列必须同时覆盖三种。这是第 29 章
“同名歧义是 schema 事实”的加强版。

### 8. `Body` / `FunctionName` 是代码注入点

`TIIPredicate` 的 `Body` 是 `MCStatement`，emitter 直接
`PE.expandStatement(OS, Rec->getValueAsDef("Body"))` 展开成目标 C++；
`FunctionName` 决定生成的 `InstrInfo` 方法名。和第 29 章的
`AltOrderSelect` 一样，`code` 字段不是数据，不能当普通字符串看。

### 9. 一个后端可以没有“名字即 API”

`register-info` 硬编码了 `GeneralPurposeRegisters` 等 10 个名字；
`instr-info` 一个都没有——它只比较目标名和 opcode 名，不比较记录名。
所以契约块里“名字即 API”一节是**可选的**，不是每个后端都有。

## 构建与验证

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop
T="$W/llvm/utils/TableGen/dsl-surface.py"
DOC="$W/llvm/lib/Target/RVMin/docs/30-instr-info-dsl.md"
TD="$W/llvm/lib/Target/RVMin/RVMin.td"

# 1. 统计（默认仍是 register-info，必须显式 --backend）
python3 "$T" --backend instr-info

# 2. 只看矩阵
python3 "$T" --backend instr-info --matrix

# 3. 文档四块一次校验
python3 "$T" --backend instr-info --check "$DOC" --usage "$TD"

# 4. emitter 行号漂移后重新生成
python3 "$T" --backend instr-info --update "$DOC" --usage "$TD"

# 5. 跨后端回归：第 29 章不能因为重构而失效
python3 "$T" --check "$W/llvm/lib/Target/RVMin/docs/29-register-info-dsl.md" \
  --usage "$W/llvm/lib/Target/RVMin/RVMinRegisterInfo.td" \
          "$W/llvm/lib/Target/RVMin/RVMinCallingConv.td"

# 6. 全量门禁
"$W/llvm/lib/Target/RVMin/scripts/rvmin-verify-all.sh" "$B/bin"
```

## 预期输出

```text
$ python3 dsl-surface.py --backend instr-info
instr-info: {'roots': 6, 'fields': 111, 'consumed': 85, 'unused': 26,
 'stages': {'schema': 26, 'derive': 74, 'project': 5, 'derive+project': 6}}

$ python3 dsl-surface.py --backend instr-info --check ... --usage ...
dsl-surface: OK (instr-info: 85/111 fields consumed [74 derive, 5 project, 6 both], 0 magic names, 8 derivations)

$ python3 dsl-surface.py --check docs/29-register-info-dsl.md --usage ...
dsl-surface: OK (register-info: 44/59 fields consumed [39 derive, 4 project, 1 both], 10 magic names, 11 derivations)

$ scripts/rvmin-verify-all.sh "$B/bin"
...
ALL GATES PASS (run rvmin-run.sh separately for the QEMU check)
```

## 常见坑

1. **`--backend` 必须显式传。** 默认是 `register-info`；忘了它就会拿错
   注解表，`--check` 报大量“未注解”或“注解过期”。
2. **不要把 `Instruction` 加进 `roots_from` 的扫描文件。** 它是在
   `CodeGenTarget.cpp` 里枚举的，但该文件还包含 `Target`、`AsmParser` 等根；
   用 `extra_roots` 精确补一个根。
3. **`TIIPredicate` 需要 `TargetInstrPredicate.td`。** 只给 `Target.td` 会
   得到 `<unknown>` 定义，继承字段全部消失。
4. **无 body 声明要区分“前向声明”和“继承别名”。** 前者不能覆盖真实定义，
   后者必须记录基类，否则字段继承链断裂。
5. **`Inst` / `Num` 是目标自定义字段。** `--usage` 标成“契约外”是正确行为；
   只有读它们的 C++ 才需要保证语义，通用 schema 不负责。
6. **不要跨后端比较字段计数。** `register-info` 的 59 和 `instr-info` 的 111
   是两个不同 schema；有意义的比较是“消费/未消费比例”和“stage 分布”。
7. **`instr-info` 的 `MAGIC_NAMES` 为空是有意的。** 空表意味着这个后端没有
   硬编码记录名，不是脚本漏配。

## 练习

1. 给 `asm-writer` 增加一个 `Backend` 条目，先只填 `sources` / `roots_from`，
   跑 `--json` 看 `missing_annotations` 有多少，再决定补注解的顺序。
2. 把 `instr-info` 的 `OperandType` 枚举条件（仅 X86）做成契约里的一个
   `condition` 字段，让矩阵也能表达“只在某个 namespace 下投影”。
3. 用 `--usage RVMinInstrInfo.td` 生成完整表，写脚本按
   `(field, status)` 聚合，看看哪类字段出现最多。
4. 对比 `register-info` 与 `instr-info` 的 `stage` 分布，解释为什么
   `instr-info` 的 `project` 比例更低。
5. 把两个后端的契约块合并成一个“TableGen 后端 DSL 总表”，需要解决哪些
   命名冲突？`(declaring class, field)` 主键够不够？

## 产物清单

| 文件 | 变更 |
| --- | --- |
| `llvm/utils/TableGen/dsl-surface.py` | 新增 `instr-info` Backend；注解/名字/派生/主产物按后端配置；支持无 body 继承声明与前向声明 |
| `llvm/lib/Target/RVMin/docs/30-instr-info-dsl.md` | 新增：本章（四个生成块） |
| `llvm/lib/Target/RVMin/docs/README.md` | 新增第 30 章索引行 |
