# 第 31 章：第三个后端——`-gen-asm-writer` 与同一份 `.td` 的三种投影

## 目标

第 29、30 章分别覆盖了 `-gen-register-info` 和 `-gen-instr-info`。本章加入
第三个后端 `-gen-asm-writer`，要验证的不是“再多一个后端”，而是：

> 同一行 `.td`，在三个后端里可能分别是“核心输入”“无关字段”“完全无效”。
> 契约能否把这种**投影差异**直接展示出来？

答案是能。RVMin 的 `RVMinRegisterInfo.td` 正好是一个小样本：

| `.td` 写法 | `register-info` | `instr-info` | `asm-writer` |
| --- | --- | --- | --- |
| `let AltNames = ...` | 无效 | 无效 | **project** |
| `let RegAltNameIndices = ...` | 无效 | 无效 | **project** |
| `let FallbackRegAltNameIndex = ...` | 无效 | 无效 | **project** |
| `let DwarfNumbers = ...` | 消费 | 无效 | 无效 |
| `let HWEncoding = ...` | 消费 | 无效 | 无效 |
| `let Namespace = ...` | 消费 | 消费 | 消费 |
| `field bits<5> Num` | 自定义 | 自定义 | 自定义 |

本章产出：

- `dsl-surface.py` 增加 `--backend asm-writer`；
- 契约：8 个记录类入口、130 个字段、99 个被消费、50 行投影矩阵、
  8 条派生改写、2 个硬编码名字；
- 复用第 30 章的指令注解，只补 18 个 asm-writer 特有字段；
- 证明“同一 schema、不同投影”可以用同一套 `(declaring class, field)` 主键表达。

非目标：

- 不修改 `AsmWriterEmitter` 的行为；
- 不覆盖 `-gen-asm-matcher`（它更大，留作练习）；
- 不把 99 个字段逐个讲一遍；正文只讲新增机制。

## 背景：为什么 `asm-writer` 是第三个

三个后端的 schema 关系不是并列的：

```text
Register ─┐
RegisterClass ─┤
RegAltNameIndex ─┼─→ register-info（枚举、DWARF、编码表）
Instruction ─┤
Operand ─┼─→ instr-info（opcode、MCInstrDesc、operand 表）
InstAlias ─┤
AsmWriter ─┴─→ asm-writer（打印 switch、别名、寄存器名）
```

`asm-writer` 同时消费寄存器 schema（`AltNames`、`AsmName`、`RegAltNameIndices`）
和指令 schema（`AsmString`、`Predicates`、`PrintMethod`），还多出两个自己的
记录类：

- `InstAlias`：汇编别名，字段 `AsmString` / `ResultInst` / `EmitPriority` /
  `Predicates` / `UseInstAsmMatchConverter` / `AsmVariantName`；
- `AsmWriter`：目标级打印器配置，字段 `AsmWriterClassName` / `PassSubtarget` /
  `Variant` / `UseBytecode`。

因此 `asm-writer` 的字段数（130）比 `instr-info`（111）还多，但两者共享
绝大部分指令/寄存器字段。第 30 章的注解可以直接复用，只需覆盖语义发生变化
的条目。

## 代码地图

| 文件 | 作用 |
| --- | --- |
| `llvm/utils/TableGen/AsmWriterEmitter.cpp` | 本后端 emitter；1474 行 |
| `llvm/utils/TableGen/Common/AsmWriterInst.{h,cpp}` | 把 `AsmString` 拆成打印片段 |
| `llvm/utils/TableGen/Common/CodeGenInstAlias.{h,cpp}` | `InstAlias` → `CodeGenInstAlias` |
| `llvm/utils/TableGen/Common/CodeGenInstruction.{h,cpp}` | 指令/操作数模型（与 instr-info 共享） |
| `llvm/utils/TableGen/Common/CodeGenTarget.{h,cpp}` | `getAsmWriter()`、指令/寄存器枚举 |
| `llvm/utils/TableGen/Common/CodeGenRegisters.{h,cpp}` | 寄存器、寄存器类、别名图 |
| `llvm/include/llvm/Target/Target.td` | `InstAlias` / `AsmWriter` / `Instruction` schema |
| `llvm/utils/TableGen/dsl-surface.py` | **本章改造**：第三个 Backend + 注解复用 |
| `llvm/lib/Target/RVMin/docs/31-asm-writer-dsl.md` | 本章；四个生成块由脚本生成 |

## 关键实现：第三个后端怎么接进来

### Backend 条目

```python
"asm-writer": Backend(
    name="asm-writer",
    emitter="llvm/utils/TableGen/AsmWriterEmitter.cpp",
    sources=(emitter, AsmWriterInst, CodeGenInstAlias,
             CodeGenInstruction, CodeGenTarget, CodeGenRegisters,
             CodeGenHwModes, InfoByHwMode),
    td_files=("Target.td",),
    roots_from=("AsmWriterEmitter.cpp",),
    extra_roots=(
        ("Instruction", CodeGenTarget.cpp, ...),
        ("Register", CodeGenRegisters.cpp, ...),
        ("RegisterClass", CodeGenRegisters.cpp, ...),
        ("Operand", InstrInfoEmitter.cpp, ...),
        ("RegisterOperand", InstrInfoEmitter.cpp, ...),
        ("RegAltNameIndex", RegisterInfoEmitter.cpp, ...),
        ("AsmWriter", CodeGenTarget.cpp,
         'getValueAsListOfDefs("AssemblyWriters")'),
    ),
    helper_classes=(HwMode/RegInfo/SubRegRange/DwarfRegAlias/RegClassByHwMode),
)
```

两个值得注意的点：

1. `roots_from` 只限定在 `AsmWriterEmitter.cpp`，但该文件只显式枚举了
   `InstAlias`。`Instruction`、`Register`、`RegisterClass` 是在共享层枚举的，
   `Operand`/`RegisterOperand` 甚至是在 `InstrInfoEmitter.cpp` 里枚举的。
   `extra_roots` 指向“schema 的枚举点”，而不是“本 emitter 的代码位置”。
2. `AsmWriter` 不是 `getAllDerivedDefinitions` 的根，而是
   `Target.AssemblyWriters[asmwriternum]` 选出来的目标级记录。契约必须把它
   显式补成根，否则 `AsmWriterClassName` / `Variant` / `UseBytecode` 全部丢失。

### 注解复用：instr-info 是 asm-writer 的子集

对比两个后端的被消费字段：

- `instr-info`：85 个；
- `asm-writer`：99 个；
- `asm-writer` 比 `instr-info` 多 18 个字段；
- 只有 `Body` / `FunctionName` / `Instruction` 是 instr-info 独有。

所以脚本用“复制 + 覆盖”的方式构造注解：

```python
ASM_WRITER_ANNOTATIONS = {
    k: v for k, v in INSTR_INFO_ANNOTATIONS.items()
    if k not in {"Body", "FunctionName", "Instruction"}
}
ASM_WRITER_ANNOTATIONS.update({
    "AsmString": "...",          # 语义从“只解析”升级为“打印模板”
    "Variant": "...",
    "ResultInst": "...",
    # ... 其余 15 个 asm-writer 特有字段
})
```

这不是偷懒，而是契约分层的结果：**字段语义是 schema 的属性，不是后端的属性**。
同一个 `Predicates` 在 instr-info 和 asm-writer 里都叫“谓词列表”，只是投影
方式不同；`stage` 列已经表达了投影差异，不需要复制两份语义。

### 机器契约

下面这一段由脚本生成，不要手改；改 emitter 后运行 `--update`。

<!-- BEGIN: GENERATED DSL CONTRACT -->

后端：`gen-asm-writer` · emitter：`llvm/utils/TableGen/AsmWriterEmitter.cpp`
统计：8 个记录类入口 · 130 个字段（schema 31 · derive 84 · project 9 · derive+project 6）。

**记录类入口**

| 记录类 | 定义 | 消费证据 |
| --- | --- | --- |
| `RegAltNameIndex` | `llvm/include/llvm/Target/Target.td:162` | `llvm/utils/TableGen/RegisterInfoEmitter.cpp:179` |
| `Register` | `llvm/include/llvm/Target/Target.td:173` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:1264` |
| `RegisterClass` | `llvm/include/llvm/Target/Target.td:292` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:1354` |
| `Instruction` | `llvm/include/llvm/Target/Target.td:608` | `llvm/utils/TableGen/Common/CodeGenTarget.cpp:287` |
| `Operand` | `llvm/include/llvm/Target/Target.td:1001` | `llvm/utils/TableGen/InstrInfoEmitter.cpp:456` |
| `RegisterOperand` | `llvm/include/llvm/Target/Target.td:1051` | `llvm/utils/TableGen/InstrInfoEmitter.cpp:458` |
| `InstAlias` | `llvm/include/llvm/Target/Target.td:1897` | `llvm/utils/TableGen/AsmWriterEmitter.cpp:950` |
| `AsmWriter` | `llvm/include/llvm/Target/Target.td:1931` | `llvm/utils/TableGen/Common/CodeGenTarget.cpp:194` |

**字段契约**（主键是 `(declaring class, field)`；`stage` 表示它停在哪一层，`⚠` 表示同名多声明）

| 字段 | 声明于 | 类型 | 默认值 | stage | 本后端读取点 | 语义 |
| --- | --- | --- | --- | --- | --- | --- |
| `Modes` | `HwModeSelect` | `list<HwMode>` | `Ms` | `derive` | `llvm/utils/TableGen/Common/CodeGenHwModes.cpp:40` | HwMode 列表；与 `Objects` 等长，按 ModeId 绑定。 |
| `RegSize` | `RegInfo` | `int` | `RS` | `derive` | `llvm/utils/TableGen/Common/InfoByHwMode.cpp:105` | `RegInfo` 的寄存器位数。 |
| `SpillAlignment` | `RegInfo` | `int` | `SA` | `derive` | `llvm/utils/TableGen/Common/InfoByHwMode.cpp:107` | `RegInfo` 的 spill slot 对齐。 |
| `SpillSize` | `RegInfo` | `int` | `SS` | `derive` | `llvm/utils/TableGen/Common/InfoByHwMode.cpp:106` | `RegInfo` 的 spill slot 位数。 |
| `Objects` | `RegInfoByHwMode`, `SubRegRangeByHwMode`, `RegClassByHwMode` | `list<RegInfo>` / `list<SubRegRange>` / `list<RegisterClass>` | `Ts` / `RegClasses` | `derive` ⚠ | `llvm/utils/TableGen/Common/CodeGenHwModes.cpp:41`, `llvm/utils/TableGen/Common/CodeGenTarget.cpp:119` | 按 HwMode 绑定的对象列表；`RegisterByHwMode` 是寄存器，`RegInfoByHwMode`/`SubRegRangeByHwMode` 是参数。 |
| `Offset` | `SubRegRange` | `int` | `offset` | `derive` | `llvm/utils/TableGen/Common/InfoByHwMode.cpp:189` | 子寄存器/范围的起始位；`-1` 表示不固定。 |
| `Size` | `SubRegRange`, `RegisterClass`, `Instruction` | `int` | `size` / `0` | `derive` ⚠ | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:800`, `llvm/utils/TableGen/Common/InfoByHwMode.cpp:188` | 指令编码字节数；`emitRecord` 写入 MCInstrDesc 的 Size。同名 `RegisterClass.Size` 是 spill size。 |
| `FallbackRegAltNameIndex` | `RegAltNameIndex` | `RegAltNameIndex` | `?` | `project` | `llvm/utils/TableGen/AsmWriterEmitter.cpp:784` | 备用寄存器别名索引；`AltNames` 缺失时的回退。 |
| `Namespace` | `RegAltNameIndex`, `Register`, `RegisterClass`, `Instruction` | `string` | `""` / `namespace` | `derive+project` ⚠ | `llvm/utils/TableGen/AsmWriterEmitter.cpp:740`, `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:419`, `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:58`, `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:59` …(+4) | 指令/寄存器命名空间；`Target.getInstNamespace()` 决定生成 enum 的 C++ namespace。 |
| `Aliases` | `Register` | `list<Register>` | `[]` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:193` | 寄存器别名列表；asm writer 用于别名/规范化寄存器名。 |
| `AltNames` | `Register` | `list<string>` | `altNames` | `project` | `llvm/utils/TableGen/AsmWriterEmitter.cpp:708` | 寄存器的备用汇编名列表；与 `RegAltNameIndices` 配对。 |
| `AsmName` | `Register` | `string` | `n` | `derive+project` | `llvm/utils/TableGen/AsmWriterEmitter.cpp:693`, `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:1307` | 寄存器的主汇编名；asm writer 打印寄存器操作数时使用。 |
| `CostPerUse` | `Register` | `list<int>` | `[0]` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:166`, `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:668` | 每寄存器成本序列；共享寄存器模型消费。 |
| `CoveredBySubRegs` | `Register` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:167` | 值由子寄存器决定；共享寄存器模型消费。 |
| `RegAltNameIndices` | `Register` | `list<RegAltNameIndex>` | `[]` | `project` | `llvm/utils/TableGen/AsmWriterEmitter.cpp:699` | 与 `AltNames` 一一对应的别名索引；asm writer 按索引选名字。 |
| `SubRegIndices` | `Register` | `list<SubRegIndex>` | `[]` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:175`, `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:632` | 子寄存器索引；与 `SubRegs` 一一对应。 |
| `SubRegs` | `Register` | `list<Register>` | `[]` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:176`, `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:634` | 子寄存器列表；共享寄存器模型消费。 |
| `isArtificial` | `Register` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:170` | 合成寄存器标记；asm writer 特殊处理。 |
| `isConstant` | `Register` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:168` | 常量寄存器标记；asm writer/寄存器模型消费。 |
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
| `AsmString` | `Instruction`, `InstAlias` | `string` | `""` / `Asm` | `derive` ⚠ | `llvm/utils/TableGen/Common/CodeGenInstAlias.cpp:137`, `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:420` | 汇编模板；asm writer 解析 `$operand`/`{a\|b}` 并生成打印 switch。 |
| `Constraints` | `Instruction` | `string` | `""` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:480` | 内联汇编约束字符串；`ParseConstraints` 为每个操作数填约束位。 |
| `Defs` | `Instruction` | `list<Register>` | `[]` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:472` | 隐式定义寄存器列表；合并进 `ImplicitDefs`，发射到 `ImplicitLists`。 |
| `FastISelShouldIgnore` | `Instruction` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:453` | FastISel 忽略该指令；只保存在 `CodeGenInstruction`，不进入 MCInstrDesc。 |
| `InOperandList` | `Instruction` | `dag` | `（未设默认值）` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:43` | 输入操作数 dag；展开成 use 列表，参与操作数编号与 `MIOperandInfo` 解析。 |
| `OutOperandList` | `Instruction` | `dag` | `（未设默认值）` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:29` | 输出操作数 dag；`CodeGenInstruction` 展开成 def 列表，决定 `NumDefs`。 |
| `Predicates` | `Instruction`, `InstAlias` | `list<Predicate>` | `[]` | `derive+project` ⚠ | `llvm/utils/TableGen/AsmWriterEmitter.cpp:1129`, `llvm/utils/TableGen/Common/CodeGenHwModes.cpp:27` | 指令/别名谓词列表；共享层解析，asm writer 用于打印条件。 |
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
| `MCOperandPredicate` | `Operand` | `code` | `（未设默认值）` | `project` | `llvm/utils/TableGen/AsmWriterEmitter.cpp:1437` | `Operand` 的 MC operand 谓词；asm writer 判断能否打印。 |
| `MIOperandInfo` | `Operand` | `dag` | `(ops)` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstAlias.cpp:32`, `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:97` | 复合操作数的子操作数 dag；`CollectOperandInfo` 用它展开 MC operand 列表。 |
| `OperandType` | `Operand`, `RegisterOperand` | `string` | `"OPERAND_UNKNOWN"` / `"OPERAND_REGISTER"` | `derive+project` ⚠ | `llvm/utils/TableGen/AsmWriterEmitter.cpp:1033`, `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:88`, `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:93` | MC operand 的 C++ 类型名；`CodeGenInstruction` 存入 `OperandType`。 |
| `PrintMethod` | `Operand`, `RegisterOperand` | `string` | `"printOperand"` / `pm` | `derive+project` ⚠ | `llvm/utils/TableGen/AsmWriterEmitter.cpp:1031`, `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:87`, `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:92` | 自定义打印函数名；`CodeGenInstruction` 保存，供 asm writer 调用。 |
| `RegClass` | `RegisterOperand` | `RegisterClassLike` | `regclass` | `derive` | `llvm/utils/TableGen/Common/CodeGenTarget.cpp:274`, `llvm/utils/TableGen/Common/InfoByHwMode.cpp:224` | 操作数绑定的寄存器类；解析为 `RegisterClass` 并用于寄存器操作数信息。 |
| `EmitPriority` | `InstAlias` | `int` | `Emit` | `project` | `llvm/utils/TableGen/AsmWriterEmitter.cpp:951` | `InstAlias` 的打印优先级；0 禁用别名打印，1 在无冲突时启用。 |
| `ResultInst` | `InstAlias` | `dag` | `Result` | `derive+project` | `llvm/utils/TableGen/AsmWriterEmitter.cpp:955`, `llvm/utils/TableGen/Common/CodeGenInstAlias.cpp:136` | `InstAlias` 的目标指令 dag；决定别名展开成哪个 MCInst。 |
| `AsmWriterClassName` | `AsmWriter` | `string` | `"InstPrinter"` | `project` | `llvm/utils/TableGen/AsmWriterEmitter.cpp:419`, `llvm/utils/TableGen/AsmWriterEmitter.cpp:581`, `llvm/utils/TableGen/AsmWriterEmitter.cpp:736`, `llvm/utils/TableGen/AsmWriterEmitter.cpp:942` | `AsmWriter` 的 C++ 类名后缀；生成 `<Target><ClassName>`。 |
| `PassSubtarget` | `AsmWriter` | `int` | `0` | `project` | `llvm/utils/TableGen/AsmWriterEmitter.cpp:420`, `llvm/utils/TableGen/AsmWriterEmitter.cpp:582`, `llvm/utils/TableGen/AsmWriterEmitter.cpp:944` | 是否把 `MCSubtargetInfo` 传给 print 方法；影响生成函数签名。 |
| `UseBytecode` | `AsmWriter` | `bit` | `0` | `project` | `llvm/utils/TableGen/AsmWriterEmitter.cpp:583` | 为 true 时生成 bytecode 表 + 解释器，而不是大 switch。 |
| `Variant` | `AsmWriter` | `int` | `0` | `project` | `llvm/utils/TableGen/AsmWriterEmitter.cpp:943`, `llvm/utils/TableGen/AsmWriterEmitter.cpp:1451` | 汇编变体编号；展开 `{a\|b\|c}` 时选择第几个分支。 |
| `AssemblyWriters` | `Target` | `list<AsmWriter>` | `[DefaultAsmWriter]` | `derive` | `llvm/utils/TableGen/Common/CodeGenTarget.cpp:194` | 目标级 `AsmWriter` 记录列表；`-asmwriternum` 选择其中一个。 |

`stage`：`schema` 仅声明 · `derive` 只被共享派生层读 · `project` 只被本 emitter 读 · `derive+project` 两者都读。

**声明了但本后端不读的字段**

| 字段 | 声明于 | 类型 | 归属 |
| --- | --- | --- | --- |
| `DwarfNumbers` | `Register` | `list<int>` | DWARF 编号；`-gen-register-info` 消费。 |
| `HWEncoding` | `Register` | `bits<16>` | 寄存器编码；`-gen-register-info` 消费。 |
| `PositionOrder` | `Register` | `int` | 寄存器枚举顺序；核心排序器消费。 |
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
| `AsmVariantName` | `Instruction`, `InstAlias` | `string` | asm variant 选择。 |
| `CodeSize` | `Instruction` | `int` | isel 的 code size 排序；`-gen-dag-isel` 消费。 |
| `EncodingInfos` | `Instruction` | `EncodingByHwMode` | 按 HwMode 的编码；encoder/decoder 消费。 |
| `GISelMatchGenericTypes` | `Instruction` | `bit` | GlobalISel 泛型类型匹配。 |
| `HasPositionOrder` | `Instruction` | `bit` | asm matcher 的位置排序。 |
| `Itinerary` | `Instruction` | `InstrItinClass` | 旧调度模型；`-gen-instr-info` 不读。 |
| `Pattern` | `Instruction` | `list<dag>` | isel pattern；`-gen-dag-isel` 消费。 |
| `PostEncoderMethod` | `Instruction` | `string` | 编码后处理函数。 |
| `SchedRW` | `Instruction` | `list<SchedReadWrite>` | 新调度模型读写资源。 |
| `TwoOperandAliasConstraint` | `Instruction` | `string` | 两操作数 alias 约束；asm matcher 消费。 |
| `UseLogicalOperandMappings` | `Instruction` | `bit` | 逻辑操作数映射开关；`-gen-instr-info` 消费。 |
| `UseNamedOperandTable` | `Instruction` | `bit` | 命名操作数表开关；`-gen-instr-info` 消费。 |
| `isAsmParserOnly` | `Instruction` | `bit` | 仅汇编器使用，codegen 不可见。 |
| `supportsDeactivationSymbol` | `Instruction` | `bit` | 指令级 deactivation 标记；其它后端消费。 |
| `ParserMatchClass` | `Operand`, `RegisterOperand` | `AsmOperandClass` | asm parser 匹配类。 |
| `Type` | `Operand` | `ValueType` | 操作数 ValueType；isel/asm 消费。 |
| `GIZeroRegister` | `RegisterOperand` | `Register` | GlobalISel 零寄存器。 |
| `UseInstAsmMatchConverter` | `InstAlias` | `bit` | 别名是否复用目标的 AsmMatchConverter；asm matcher 消费。 |

**名字即 API**（不经过字段查找的硬编码名字）

| 名字 | 出现位置 | 作用 |
| --- | --- | --- |
| `NoRegAltName` | `llvm/utils/TableGen/AsmWriterEmitter.cpp:692` | 缺少 AltName 时的哨兵；asm writer 回退到主 `AsmName`。 |
| `PHI` | `llvm/utils/TableGen/AsmWriterEmitter.cpp:1457` | asm writer 跳过 PHI 的别名排序/打印。 |

<!-- END: GENERATED DSL CONTRACT -->

### 跨后端投影矩阵

矩阵第一次同时出现 `register-info`、`instr-info`、`asm-writer` 三个后端。
重点看 `AltNames` / `RegAltNameIndices` / `AsmName`：它们在 register-info 里
是“未消费”，在 asm-writer 里是核心输入。

<!-- BEGIN: GENERATED PROJECTION MATRIX -->

只列“本后端之外还有读者”或“同名多声明”的字段；`shared` 表示共享派生层也读它。

| 字段 | 声明于 | shared | 其它 `-gen-*` | helper | 同名 |
| --- | --- | --- | --- | --- | --- |
| `Objects` | `RegInfoByHwMode`, `SubRegRangeByHwMode`, `RegClassByHwMode` | ✓ | `instr-info` | — | ⚠ |
| `Size` | `SubRegRange`, `RegisterClass`, `Instruction` | ✓ | `asm-matcher`, `callingconv`, `compress-inst-emitter`, `instr-info` | `helper:DecoderEmitter`, `helper:X86RecognizableInstr` | ⚠ |
| `Namespace` | `RegAltNameIndex`, `Register`, `RegisterClass`, `Instruction` | ✓ | `asm-matcher`, `emitter`, `fast-isel`, `pseudo-lowering`, `register-info`, `subtarget` | — | ⚠ |
| `AltNames` | `Register` | — | `asm-matcher` | — | — |
| `AsmName` | `Register` | ✓ | `asm-matcher` | — | — |
| `DwarfNumbers` | `Register` | — | `register-info` | — | — |
| `HWEncoding` | `Register` | — | `register-info` | — | — |
| `RegAltNameIndices` | `Register` | — | `asm-matcher` | — | — |
| `DecoderMethod` | `DAGOperand`, `InstructionEncoding` | ✓ | — | — | ⚠ |
| `hasCompleteDecoder` | `DAGOperand`, `InstructionEncoding` | ✓ | — | — | ⚠ |
| `Alignment` | `RegisterClass` | ✓ | — | `helper:X86RecognizableInstr` | — |
| `CoerceFromRegClass` | `RegisterClass` | — | `asm-matcher` | — | — |
| `DiagnosticString` | `RegisterClass` | — | `asm-matcher` | — | — |
| `DiagnosticType` | `RegisterClass` | — | `asm-matcher` | — | — |
| `TSFlags` | `RegisterClass`, `Instruction` | ✓ | `instr-info` | — | ⚠ |
| `DwarfAlias` | `DwarfRegAlias` | — | `register-info` | — | — |
| `DecoderNamespace` | `InstructionEncoding` | ✓ | — | `helper:DecoderEmitter` | — |
| `AsmMatchConverter` | `Instruction` | — | `asm-matcher` | — | — |
| `AsmString` | `Instruction`, `InstAlias` | ✓ | `insn-directive-match-table` | — | ⚠ |
| `AsmVariantName` | `Instruction`, `InstAlias` | — | `asm-matcher`, `x86-mnemonic-tables` | — | ⚠ |
| `CodeSize` | `Instruction` | — | `dag-isel` | — | — |
| `Constraints` | `Instruction` | ✓ | `instr-docs` | — | — |
| `Defs` | `Instruction` | ✓ | `global-isel-combiner`, `x86-fold-tables`, `x86-instr-mapping` | — | — |
| `EncodingInfos` | `Instruction` | ✓ | `emitter` | `helper:DecoderEmitter` | — |
| `GISelMatchGenericTypes` | `Instruction` | — | `global-isel` | — | — |
| `HasPositionOrder` | `Instruction` | — | `asm-matcher` | — | — |
| `InOperandList` | `Instruction` | ✓ | `insn-directive-match-table`, `x86-fold-tables` | — | — |
| `OutOperandList` | `Instruction` | ✓ | `insn-directive-match-table`, `x86-fold-tables` | — | — |
| `PostEncoderMethod` | `Instruction` | ✓ | `emitter` | — | — |
| `Predicates` | `Instruction`, `InstAlias` | ✓ | `asm-matcher`, `compress-inst-emitter`, `global-isel-combiner`, `instr-docs`, `instr-info`, `macro-fusion-pred`, `subtarget`, `x86-instr-mapping` | `helper:DecoderEmitter`, `helper:X86RecognizableInstr` | ⚠ |
| `TwoOperandAliasConstraint` | `Instruction` | — | `asm-matcher` | — | — |
| `UseLogicalOperandMappings` | `Instruction` | — | `instr-info` | — | — |
| `UseNamedOperandTable` | `Instruction` | — | `instr-info` | — | — |
| `Uses` | `Instruction` | ✓ | `x86-fold-tables` | — | — |
| `isAsmParserOnly` | `Instruction` | — | `x86-fold-tables`, `x86-instr-mapping` | `helper:DecoderEmitter`, `helper:X86RecognizableInstr` | — |
| `isCodeGenOnly` | `Instruction` | ✓ | — | `helper:DecoderEmitter`, `helper:X86RecognizableInstr` | — |
| `isPseudo` | `Instruction` | ✓ | `searchable-tables`, `x86-fold-tables` | — | — |
| `supportsDeactivationSymbol` | `Instruction` | — | — | `helper:DAGISelMatcherEmitter` | — |
| `EncoderMethod` | `Operand`, `RegisterOperand` | ✓ | — | — | ⚠ |
| `MIOperandInfo` | `Operand` | ✓ | `instr-info` | `helper:DAGISelMatcherGen` | — |
| `OperandType` | `Operand`, `RegisterOperand` | ✓ | `global-isel-combiner` | `helper:X86RecognizableInstr` | ⚠ |
| `ParserMatchClass` | `Operand`, `RegisterOperand` | — | `asm-matcher`, `insn-directive-match-table` | — | ⚠ |
| `PrintMethod` | `Operand`, `RegisterOperand` | ✓ | — | — | ⚠ |
| `Type` | `Operand` | ✓ | `global-isel-combiner`, `x86-fold-tables`, `x86-instr-mapping` | — | — |
| `GIZeroRegister` | `RegisterOperand` | — | `global-isel` | — | — |
| `RegClass` | `RegisterOperand` | ✓ | `asm-matcher`, `fast-isel`, `global-isel`, `instr-info` | `helper:DAGISelMatcherGen`, `helper:X86RecognizableInstr` | — |
| `ResultInst` | `InstAlias` | ✓ | `pseudo-lowering` | — | — |
| `UseInstAsmMatchConverter` | `InstAlias` | — | `asm-matcher` | — | — |
| `PassSubtarget` | `AsmWriter` | — | `compress-inst-emitter` | — | — |
| `Variant` | `AsmWriter` | — | `asm-matcher`, `x86-fold-tables`, `x86-mnemonic-tables` | — | — |

矩阵覆盖 50 个字段（共 130 个）。
<!-- END: GENERATED PROJECTION MATRIX -->

### 派生层改写（Record → 投影之间的模型变换）

`asm-writer` 的派生层有两处特别典型：

1. `InstAlias.ResultInst` 被展开成别名操作数；
2. `Instruction.AsmString` 被拆成 literal / operand / variant 片段。

这两步都不在 `.td` 里，也不在最终 C++ 里，而是在投影前的共享模型中完成。

<!-- BEGIN: GENERATED DERIVATIONS -->

Record 里只有声明；这些是共享 C++ 在投影之前对语义的改写。

| 改写 | Schema 键 | 证据 | 说明 |
| --- | --- | --- | --- |
| `alias construction` | `InstAlias` | `llvm/utils/TableGen/Common/CodeGenInstAlias.cpp:134` | 把 `ResultInst` dag 展开成别名操作数，并解析寄存器/立即数。 |
| `asm string parsing` | `Instruction.AsmString` | `llvm/utils/TableGen/Common/AsmWriterInst.cpp:49` | 把汇编模板拆成 literal/operand/variant 片段，供打印 switch 使用。 |
| `asm writer selection` | `Target.AssemblyWriters` | `llvm/utils/TableGen/Common/CodeGenTarget.cpp:194` | 按 `-asmwriternum` 选择目标的一个 `AsmWriter`。 |
| `instruction ordering` | `Instruction` | `llvm/utils/TableGen/Common/CodeGenTarget.cpp:287` | 按记录名排序，决定 opcode 编号与打印表顺序。 |
| `register ordering` | `Register` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:1264` | 按 `PositionOrder`/名字排序，决定寄存器枚举与名字表顺序。 |
| `alt-name resolution` | `Register.RegAltNameIndices` | `llvm/utils/TableGen/AsmWriterEmitter.cpp:738`, `llvm/utils/TableGen/Common/CodeGenTarget.h:138` | 把 `RegAltNameIndices` 解析成实际的别名索引表。 |
| `hw-mode selection` | `HwModeSelect.Objects` | `llvm/utils/TableGen/Common/CodeGenHwModes.cpp:42` | 按 ModeId 把 `Objects[i]` 绑定到 `Modes[i]`。 |
| `operand type resolution` | `RegisterOperand.RegClass` | `llvm/utils/TableGen/Common/CodeGenTarget.cpp:274`, `llvm/utils/TableGen/Common/InfoByHwMode.cpp:224` | 把 `RegisterOperand` 解析到具体 `RegisterClass`。 |
<!-- END: GENERATED DERIVATIONS -->

### RVMin 的真实用法对照

同一个 `RVMinRegisterInfo.td`，在 `asm-writer` 下的状态与第 29 章完全不同：

<!-- BEGIN: GENERATED TD USAGE -->

| 文件:行 | 字段 | 契约状态 |
| --- | --- | --- |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:12` | `FallbackRegAltNameIndex` | 消费（project） |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:17` | `Namespace` | 消费（derive+project） |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:18` | `HWEncoding` | 寄存器编码；`-gen-register-info` 消费。 |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:19` | `AltNames` | 消费（project） |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:20` | `DwarfNumbers` | DWARF 编号；`-gen-register-info` 消费。 |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:25` | `RegAltNameIndices` | 消费（project） |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:86` | `Namespace` | 消费（derive+project） |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:87` | `HWEncoding` | 寄存器编码；`-gen-register-info` 消费。 |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:88` | `AltNames` | 消费（project） |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:89` | `DwarfNumbers` | DWARF 编号；`-gen-register-info` 消费。 |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:92` | `RegAltNameIndices` | 消费（project） |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:147` | `DecoderMethod` | decoder 自定义方法。 |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:158` | `DecoderMethod` | decoder 自定义方法。 |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:16` | `Num` | 自定义字段（需自行保证 name-based 读取方存在） |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:85` | `Num` | 自定义字段（需自行保证 name-based 读取方存在） |

共 15 处使用；契约外 0 处。
<!-- END: GENERATED TD USAGE -->

对照第 29 章的表：

- `AltNames` / `RegAltNameIndices` / `FallbackRegAltNameIndex` 从“asm writer /
  asm matcher（无效）”变成 `project`；
- `DwarfNumbers` / `HWEncoding` 从“消费”变成“register-info 消费”；
- `Namespace` 三个后端都消费，是真正的共享字段；
- `Num` 始终是 RVMin 自定义字段。

这张表就是本章最直接的结论：**不是字段变了，是投影变了**。

## 深读：`asm-writer` 暴露出的新规则

### 1. 同一行 `.td`，三种 stage

`let AltNames = [...]` 在 register-info 里没有读取点，在 instr-info 里也没有，
在 asm-writer 里是 emitter 独占的 `project`。契约不判断“这行写得对不对”，
只判断“哪个后端读它”。这就是投影矩阵存在的意义。

### 2. `InstAlias` 是独立的 schema

`InstAlias` 不属于 `Instruction` 继承链：

```tablegen
class InstAlias<string Asm, dag Result, int Emit = 1, string VariantName = ""> {
  string AsmString = Asm;
  dag ResultInst = Result;
  int EmitPriority = Emit;
  list<Predicate> Predicates = [];
  bit UseInstAsmMatchConverter = true;
  string AsmVariantName = VariantName;
}
```

`AsmString` 是匹配模板，`ResultInst` 是展开后的 MCInst，`EmitPriority` 决定
打印优先级。它们和 `Instruction` 的同名字段语义不同，但字段名相同，所以契约
表会出现 `⚠` 同名标记。

### 3. `AsmWriter` 是目标级记录，不是根类枚举

`Target.AssemblyWriters` 是一个 `list<AsmWriter>`；`-asmwriternum=N` 选择其中
一个。契约覆盖所有 `AsmWriter` 定义，但一次生成只投影一个。这也是为什么
`AsmWriter` 必须用 `extra_roots` 显式补根。

### 4. `AsmString` 的 `$operand` / `{a|b}` 语法

`AsmWriterInst` 把模板拆成三类片段：

- literal：原样输出；
- operand：`$rs1` / `${rs1:print}`，绑定操作数打印方法；
- variant：`{a|b|c}`，由 `AsmWriter.Variant` 选择分支。

所以 `AsmString` 不是普通字符串，而是 asm-writer 的“程序”。

### 5. `NoRegAltName` 与 `PHI` 是两个硬编码名字

- `NoRegAltName`：`AltNames` 缺失时的哨兵，asm writer 回退到主 `AsmName`；
- `PHI`：asm writer 在别名排序/打印时跳过 PHI。

它们不在字段契约里，只能靠 `MAGIC_NAMES` 表捕获。第 29 章的
`GeneralPurposeRegisters` 是同类问题。

### 6. `Variant` 与 `AsmVariantName` 不是一回事

- `AsmWriter.Variant` 是整数，决定 `{a|b|c}` 选哪个分支；
- `Instruction.AsmVariantName` / `InstAlias.AsmVariantName` 是字符串，决定
  使用哪个汇编变体。

同名不同类、同义不同名，这两类混淆正是契约要消灭的。

### 7. `UseBytecode` 是整表策略开关

为 true 时，asm writer 不生成大 switch，而是生成 bytecode 表 + 解释器循环。
一个 bit 改变整个产物的形状，和第 30 章的 `UseNamedOperandTable` /
`UseLogicalOperandMappings` 属于同一类“投影级开关”。

### 8. `MCOperandPredicate` 是 asm-writer 的 project 字段

`Operand.MCOperandPredicate` 在 instr-info 里未消费，在 asm-writer 里被
emitter 直接读取，用于判断操作数能否打印。它再次说明：**未消费不是死字段，
只是不属于当前投影**。

### 9. 三个后端的 stage 分布对比

| 后端 | schema | derive | project | derive+project | 被消费/声明 |
| --- | --- | --- | --- | --- | --- |
| `register-info` | 15 | 39 | 4 | 1 | 44/59 |
| `instr-info` | 26 | 74 | 5 | 6 | 85/111 |
| `asm-writer` | 31 | 84 | 9 | 6 | 99/130 |

规律很稳定：**derive 层占大头，project 层很小**。这就是“只看 emitter 会漏掉
大部分语义”的量化证据。

## 构建与验证

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop
T="$W/llvm/utils/TableGen/dsl-surface.py"
DOC="$W/llvm/lib/Target/RVMin/docs/31-asm-writer-dsl.md"
TD="$W/llvm/lib/Target/RVMin/RVMinRegisterInfo.td"

# 1. 统计
python3 "$T" --backend asm-writer

# 2. 只看矩阵（应同时出现 register-info / instr-info / asm-writer）
python3 "$T" --backend asm-writer --matrix

# 3. 文档四块一次校验
python3 "$T" --backend asm-writer --check "$DOC" --usage "$TD"

# 4. 行号漂移后重新生成
python3 "$T" --backend asm-writer --update "$DOC" --usage "$TD"

# 5. 前两章回归
python3 "$T" --check "$W/llvm/lib/Target/RVMin/docs/29-register-info-dsl.md" \
  --usage "$W/llvm/lib/Target/RVMin/RVMinRegisterInfo.td" \
          "$W/llvm/lib/Target/RVMin/RVMinCallingConv.td"
python3 "$T" --backend instr-info --check \
  "$W/llvm/lib/Target/RVMin/docs/30-instr-info-dsl.md" \
  --usage "$W/llvm/lib/Target/RVMin/RVMin.td"

# 6. 全量门禁
"$W/llvm/lib/Target/RVMin/scripts/rvmin-verify-all.sh" "$B/bin"
```

## 预期输出

```text
$ python3 dsl-surface.py --backend asm-writer
asm-writer: {'roots': 8, 'fields': 130, 'consumed': 99, 'unused': 31,
 'stages': {'schema': 31, 'derive': 84, 'project': 9, 'derive+project': 6}}

$ python3 dsl-surface.py --backend asm-writer --check ... --usage ...
dsl-surface: OK (asm-writer: 99/130 fields consumed [84 derive, 9 project, 6 both], 2 magic names, 8 derivations)

$ scripts/rvmin-verify-all.sh "$B/bin"
...
ALL GATES PASS (run rvmin-run.sh separately for the QEMU check)
```

## 常见坑

1. **`extra_roots` 可以指向别的 emitter。** `Operand` / `RegisterOperand` 的
   `getAllDerivedDefinitions` 在 `InstrInfoEmitter.cpp` 里，但它们的字段属于
   asm-writer 的 schema。指向“枚举点”而不是“本文件”是正确做法。
2. **`AsmWriter` 不是 `getAllDerivedDefinitions` 根。** 它通过
   `Target.AssemblyWriters` 列表选择；漏掉它，`Variant` / `UseBytecode`
   会静默消失。
3. **复用注解时必须覆盖语义变化的字段。** `AsmString` 在 instr-info 里只是
   被解析，在 asm-writer 里是打印模板；直接复制会得到错误说明。
4. **`MAGIC_NAMES` 的字符串可能移动。** `PHI` / `NoRegAltName` 是 emitter
   里的字面量，重构后行号会变，极端情况下字符串会消失；`--check` 会失败，
   这是有意的。
5. **`--usage` 的“契约状态”随后端变化。** 同一行 `AltNames` 在三个后端下
   分别是“无效 / 无效 / project”，不要拿一张表当绝对结论。
6. **不要用 asm-writer 的 130 字段去推断 instr-info 的 schema。** 两者的
   root 集合不同；字段计数只有加上 `declared_in` 才有意义。
7. **`-asmwriternum` 只影响一次投影。** 契约覆盖所有 `AsmWriter` 定义，
   但生成时只选一个；对比产物时需要显式传 `-asmwriternum`。

## 练习

1. 给 `asm-matcher` 增加 `Backend` 条目。先只填 `roots_from`，看
   `missing_annotations` 有多少，再决定复用哪些注解。
2. 把 `AsmWriter.Variant` 的 `{a|b|c}` 展开规则做成派生改写表的一项，
   需要什么正则才能稳定命中？
3. 用 `--usage RVMinInstrInfo.td` 生成 asm-writer 的完整用法表，统计
   `PrintMethod` / `AsmString` / `Predicates` 各出现多少次。
4. 对比三个后端的 `MAGIC_NAMES`，解释为什么 asm-writer 只有 2 个而
   register-info 有 10 个。
5. 设计一个“跨后端 status 矩阵”：行是 RVMin `.td` 的每个 `let`，列是
   三个后端，单元格是 stage。需要给脚本加什么输出模式？

## 产物清单

| 文件 | 变更 |
| --- | --- |
| `llvm/utils/TableGen/dsl-surface.py` | 新增 `asm-writer` Backend；复用 instr-info 注解并补 18 个字段；2 个魔术名；8 条派生改写 |
| `llvm/lib/Target/RVMin/docs/31-asm-writer-dsl.md` | 新增：本章（四个生成块） |
| `llvm/lib/Target/RVMin/docs/README.md` | 新增第 31 章索引行 |
| `llvm/lib/Target/RVMin/scripts/rvmin-verify-all.sh` | 第 7 项增加 asm-writer 契约校验 |
