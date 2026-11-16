# 第 36 章：第五个后端——`-gen-dag-isel` 与 pattern schema

## 目标

第 34 章把契约推进到非指令记录，但 asm-matcher 的输入仍然是“字段”。
`-gen-dag-isel` 不一样：它的核心输入是 **dag 树**（`Pattern` / `PatFrags` /
`ComplexPattern`），字段只是在描述树的形状和约束。

要验证的问题：

> 当后端的主要输入是嵌套 dag、而不是标量字段时，`(declaring class, field)`
> 契约还能不能给出可读的影响面？

答案是能，但投影形态完全不同：

| 后端 | roots | 字段 | 消费 | derive / project / both | schema |
| --- | --- | --- | --- | --- | --- |
| `register-info` | 8 | 59 | 44 | 39 / 4 / 1 | 15 |
| `instr-info` | 6 | 111 | 85 | 74 / 5 / 6 | 26 |
| `asm-writer` | 8 | 130 | 99 | 84 / 9 / 6 | 31 |
| `asm-matcher` | 15 | 166 | 142 | 95 / 42 / 5 | 24 |
| `dag-isel` | **14** | **186** | **136** | **135 / 1 / 0** | **50** |

`dag-isel` 有两个极端：

- `project` 只有 **1**（`CodeSize`）：`DAGISelEmitter.cpp` 只是一层薄壳；
- `derive` 有 **135**：几乎所有读取都发生在共享分析器
  `CodeGenDAGPatterns.cpp`。

本章产出：

- `dsl-surface.py` 增加 `--backend dag-isel`；
- 契约：14 个记录类入口、186 个字段、136 个被消费、64 行投影矩阵、
  7 条派生改写、6 个硬编码名字；
- 注解复用 instr-info，补 56 个 pattern/SDNode 字段（含 `ImmLeaf` 的 2 个）；
- 脚本增强：magic name 从“只扫 emitter”改为“扫整个 source set”；
- 第 32 章 status 矩阵与第 33 章 `--effect` 自动变成六后端（第 37 章再加 `fast-isel`）。

非目标：

- 不修改 `DAGISelEmitter` / `CodeGenDAGPatterns` 的行为；
- 不覆盖 `-gen-fast-isel` / `-gen-disassembler`；
- 不把 186 个字段逐个讲一遍；正文只讲 pattern schema 的新机制。

## 背景：为什么 dag-isel 是第五个

前四个后端的输入可以画成“记录 → 字段”：

```text
Instruction / Register / AsmParser → 字段 → 生成的表
```

dag-isel 多了一层“树”：

```text
Pattern.PatternToMatch ─┐
PatFrags.Fragments      ├─→ TreePattern → 匹配器状态机
ComplexPattern.Ty       │
SDNode.TypeProfile     ─┘
```

这带来三个差异：

1. **dag 是字段的值，不是字段本身。** `PatternToMatch` / `ResultInstrs` /
   `Fragments` 的类型都是 `dag` 或 `list<dag>`，契约只能描述“这个字段存了一棵树”，
   树的形状由共享分析器解释。
2. **分析器比 emitter 大。** `DAGISelEmitter.cpp` 只读 `CodeSize`；真正读
   `Pattern` / `PatFrags` / `ComplexPattern` 的是
   `Common/CodeGenDAGPatterns.cpp`。
3. **类型推断是核心派生。** `SDTypeProfile` / `SDTypeConstraint` 把
   “结果/操作数个数 + 类型约束”编码进节点，分析器再推断每个操作数的
   `ValueType`。

## 代码地图

| 文件 | 作用 |
| --- | --- |
| `llvm/utils/TableGen/DAGISelEmitter.cpp` | 本后端 emitter（薄壳）；只读 `CodeSize` |
| `llvm/utils/TableGen/Common/CodeGenDAGPatterns.{h,cpp}` | pattern 解析、类型推断、匹配器构建 |
| `llvm/utils/TableGen/Common/CodeGenInstruction.{h,cpp}` | 指令/操作数模型（共享） |
| `llvm/utils/TableGen/Common/CodeGenRegisters.{h,cpp}` | 寄存器模型（`getRegBank()`） |
| `llvm/utils/TableGen/Common/CodeGenTarget.{h,cpp}` | `getInstructions()`、`ValueType` 查找 |
| `llvm/utils/TableGen/Common/SubtargetFeatureInfo.{h,cpp}` | `Predicate` → feature |
| `llvm/utils/TableGen/Basic/SDNodeProperties.{h,cpp}` | `SDNode` 属性位 |
| `llvm/include/llvm/Target/TargetSelectionDAG.td` | `SDNode` / `PatFrags` / `ComplexPattern` / `Pattern` schema |
| `llvm/include/llvm/Target/Target.td` | `Instruction` / `Operand` / `Register*` schema |
| `llvm/utils/TableGen/dsl-surface.py` | **本章改造**：第五个 Backend + 56 个注解 + `ImmLeaf` helper |
| `llvm/lib/Target/RVMin/docs/36-dag-isel-dsl.md` | 本章；四个生成块由脚本生成 |

## 关键实现：第五个后端怎么接进来

### 1. `sources` 以共享分析器为中心

```python
"dag-isel": Backend(
    name="dag-isel",
    emitter="llvm/utils/TableGen/DAGISelEmitter.cpp",
    sources=(
        (emitter, "emitter"),
        CodeGenDAGPatterns, CodeGenInstruction, CodeGenRegisters,
        CodeGenTarget, CodeGenHwModes, InfoByHwMode,
        SubtargetFeatureInfo, SDNodeProperties,
    ),
    ...
)
```

注意两点：

- **不包含 `Basic/CodeGenIntrinsics.cpp`。** `CodeGenTarget` 构造时会读全部
  intrinsic 定义；把 29 个 `Intrinsic` 字段放进来会淹没 pattern 契约。
- **包含 `SDNodeProperties.cpp`。** 它读 `SDNode.Properties`，决定属性位。

### 2. schema：`TargetSelectionDAG.td` + 一组 helper class

`td_files` 只有三个：

```python
td_files=(
    Target.td, TargetSelectionDAG.td, TargetSubtarget.td,
)
```

`TargetSelectionDAG.td` 提供 `SDNode` / `PatFrags` / `ComplexPattern` /
`Pattern` / `SDTypeProfile`；`TargetSubtarget.td` 提供 `Predicate`。

但根枚举不会自动带出两类类：

- `SDTypeProfile`：通过 `SDNode.TypeProfile` 字段引用；
- `SDTypeConstraint` 及其子类：通过 `SDTypeProfile.Constraints` 引用。

所以 `helper_classes` 显式列出：

```python
helper_classes=(
    ..., "SDTypeProfile", "SDTypeConstraint",
    "SDTCisSameAs", "SDTCisVTSmallerThanOp", "SDTCisOpSmallerThanOp",
    "SDTCisEltOfVec", "SDTCisSubVecOfVec", "SDTCVecEltisVT",
    "SDTCisSameNumEltsAs", "SDTCisSameSizeAs",
    "ValueType", "Predicate",
)
```

`ValueType` 也只通过字段引用（`ComplexPattern.Ty`、`SDTCisVT.VT`），没有
`getAllDerivedDefinitions("ValueType")`，所以必须列为 helper。

### 3. roots：14 个

| 来源 | 根 |
| --- | --- |
| `CodeGenDAGPatterns.cpp` | `SDNode`, `SDNodeXForm`, `ComplexPattern`, `PatFrags`, `OperandWithDefaultOps`, `Instruction`, `Pattern` |
| `CodeGenRegisters.cpp` | `SubRegIndex`, `Register`, `RegisterClass`, `RegisterTuples`, `RegisterCategory` |
| `extra_roots` | `Operand`, `RegisterOperand` |

`Instruction` 在 `CodeGenDAGPatterns.cpp` 里也会被枚举；`extra_roots` 里的
同名条目只是保留“schema 枚举点”的证据，二者会去重。

### 4. 注解：56 个新字段

按声明类分组：

| 声明类 | 新增字段数 | 代表字段 |
| --- | --- | --- |
| `PatFrags` | 10 | `Fragments` / `PredicateCode` / `MemoryVT` |
| `Register` 系（`Register` / `RegisterTuples` / `RegisterCategory`） | 10 | `AsmName` / `CostPerUse` / `RegAsmNames` |
| `ComplexPattern` | 7 | `SelectFunc` / `RootNodes` / `WantsRoot` |
| `SDNode` 系（`SDNode` / `SDNodeXForm`） | 5 | `Opcode` / `SDClass` / `XFormFunction` |
| `Predicate` | 4 | `CondString` / `AssemblerCondDag` |
| `Instruction` | 3 | `Pattern` / `CodeSize` / `AddedComplexity` |
| `Pattern` | 3 | `PatternToMatch` / `ResultInstrs` / `GISelShouldIgnore` |
| `SubRegIndex` | 3 | `SubRegRanges` / `ComposedOf` |
| `ImmLeaf` | 2 | `IsAPInt` / `IsAPFloat` |
| `SDTypeConstraint` 系 | 5 | `OperandNum` / `OtherOperandNum` / `VT` |
| 其它（`SDTypeProfile` / `Operand` / `OperandWithDefaultOps`） | 4 | `NumResults` / `Type` / `DefaultOps` |

### 5. magic name 改为扫描整个 source set

`set` / `node` / `srcvalue` / `null_frag` 这些名字不在
`DAGISelEmitter.cpp`，而在 `CodeGenDAGPatterns.cpp`。原脚本只扫 emitter，
所以第 36 章顺手修了这个边界：

```python
source_texts = [(rel, strip_comments((root / rel).read_text()))
                for rel, _ in backend.sources]
```

这不会改变前四个后端的输出（它们的 magic name 都只出现在 emitter），
但能让“名字即 API”覆盖共享分析器。

### 6. derivations：7 条

- `Pattern` 收集 → `TreePattern`；
- `Instruction.Pattern` 合并；
- `PatFrags.Fragments` 展开；
- `TypeInfer` 类型推断；
- `Predicate.CondString` 下沉为匹配条件；
- `SDNode` 注册（opcode/属性/profile）；
- `Operand.Type` 解析。

### 7. 投影特征：135 derive / 1 project / 0 both

这是本章最值得记住的数字：

- `project` 只有 `CodeSize`：emitter 不直接读 schema；
- `derive+project` 为 0：没有“共享层和 emitter 同时读”的字段；
- `derive` 有 135 个，覆盖 pattern、SDNode、类型约束、寄存器模型。

> `ImmLeaf` 是一个容易漏掉的边界：它的 `IsAPInt` / `IsAPFloat` /
> `FastIselShouldIgnore` 声明在**派生类**上，而 `collect_fields()` 只沿基类
> 展开。必须把 `ImmLeaf` 显式列进 `helper_classes`，这三个字段才会进入
> schema（这也是本章数字从 183 涨到 186 的原因）。

也就是说，**dag-isel 的契约主要是“共享分析器的输入面”**，而不是
“emitter 的输入面”。这正是第 29 章“derive 层”概念的最大规模样本。

## 契约与投影

下面四段由脚本生成，不要手改；改 emitter 或 `.td` 后运行 `--update`。

<!-- BEGIN: GENERATED DSL CONTRACT -->

后端：`gen-dag-isel` · emitter：`llvm/utils/TableGen/DAGISelEmitter.cpp`
统计：14 个记录类入口 · 186 个字段（schema 50 · derive 135 · project 1 · derive+project 0）。

**记录类入口**

| 记录类 | 定义 | 消费证据 |
| --- | --- | --- |
| `SubRegIndex` | `llvm/include/llvm/Target/Target.td:108` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:1256` |
| `Register` | `llvm/include/llvm/Target/Target.td:173` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:1264` |
| `RegisterClass` | `llvm/include/llvm/Target/Target.td:292` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:1354` |
| `RegisterTuples` | `llvm/include/llvm/Target/Target.td:478` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:1271`, `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:1288` |
| `RegisterCategory` | `llvm/include/llvm/Target/Target.td:506` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:1393` |
| `Instruction` | `llvm/include/llvm/Target/Target.td:608` | `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:4135` |
| `Operand` | `llvm/include/llvm/Target/Target.td:1001` | `llvm/utils/TableGen/InstrInfoEmitter.cpp:456` |
| `RegisterOperand` | `llvm/include/llvm/Target/Target.td:1051` | `llvm/utils/TableGen/InstrInfoEmitter.cpp:458` |
| `OperandWithDefaultOps` | `llvm/include/llvm/Target/Target.td:1156` | `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:3547` |
| `SDNode` | `llvm/include/llvm/Target/TargetSelectionDAG.td:375` | `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:3430` |
| `SDNodeXForm` | `llvm/include/llvm/Target/TargetSelectionDAG.td:1026` | `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:3443` |
| `PatFrags` | `llvm/include/llvm/Target/TargetSelectionDAG.td:1088` | `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:3464` |
| `Pattern` | `llvm/include/llvm/Target/TargetSelectionDAG.td:2519` | `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:4526` |
| `ComplexPattern` | `llvm/include/llvm/Target/TargetSelectionDAG.td:2537` | `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:3452` |

**字段契约**（主键是 `(declaring class, field)`；`stage` 表示它停在哪一层，`⚠` 表示同名多声明）

| 字段 | 声明于 | 类型 | 默认值 | stage | 本后端读取点 | 语义 |
| --- | --- | --- | --- | --- | --- | --- |
| `Modes` | `HwModeSelect` | `list<HwMode>` | `Ms` | `derive` | `llvm/utils/TableGen/Common/CodeGenHwModes.cpp:40` | HwMode 列表；与 `Objects` 等长，按 ModeId 绑定。 |
| `RegSize` | `RegInfo` | `int` | `RS` | `derive` | `llvm/utils/TableGen/Common/InfoByHwMode.cpp:105` | `RegInfo` 的寄存器位数。 |
| `SpillAlignment` | `RegInfo` | `int` | `SA` | `derive` | `llvm/utils/TableGen/Common/InfoByHwMode.cpp:107` | `RegInfo` 的 spill slot 对齐。 |
| `SpillSize` | `RegInfo` | `int` | `SS` | `derive` | `llvm/utils/TableGen/Common/InfoByHwMode.cpp:106` | `RegInfo` 的 spill slot 位数。 |
| `Objects` | `RegInfoByHwMode`, `SubRegRangeByHwMode`, `RegClassByHwMode` | `list<RegInfo>` / `list<SubRegRange>` / `list<RegisterClass>` | `Ts` / `RegClasses` | `derive` ⚠ | `llvm/utils/TableGen/Common/CodeGenHwModes.cpp:41`, `llvm/utils/TableGen/Common/CodeGenTarget.cpp:119` | 按 HwMode 绑定的对象列表；`RegisterByHwMode` 是寄存器，`RegInfoByHwMode`/`SubRegRangeByHwMode` 是参数。 |
| `Offset` | `SubRegRange`, `SubRegIndex` | `int` | `offset` | `derive` ⚠ | `llvm/utils/TableGen/Common/InfoByHwMode.cpp:189` | 子寄存器/范围的起始位；`-1` 表示不固定。 |
| `Size` | `SubRegRange`, `SubRegIndex`, `RegisterClass`, `Instruction` | `int` | `size` / `0` | `derive` ⚠ | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:800`, `llvm/utils/TableGen/Common/InfoByHwMode.cpp:188` | 指令编码字节数；`emitRecord` 写入 MCInstrDesc 的 Size。同名 `RegisterClass.Size` 是 spill size。 |
| `ComposedOf` | `SubRegIndex` | `list<SubRegIndex>` | `[]` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:86` | 复合子寄存器索引的组成列表。 |
| `CoveringSubRegIndices` | `SubRegIndex` | `list<SubRegIndex>` | `[]` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:99` | 覆盖该子寄存器索引的父索引列表。 |
| `Namespace` | `SubRegIndex`, `Register`, `RegisterClass`, `Instruction` | `string` | `""` / `namespace` | `derive` ⚠ | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:419`, `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:58`, `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:59`, `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:796` …(+3) | 指令/寄存器命名空间；`Target.getInstNamespace()` 决定生成 enum 的 C++ namespace。 |
| `SubRegRanges` | `SubRegIndex` | `SubRegRangeByHwMode` | `（未设默认值）` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:61` | 按 HwMode 的子寄存器范围。 |
| `Aliases` | `Register` | `list<Register>` | `[]` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:193` | 寄存器别名列表；共享寄存器模型合并别名。 |
| `AsmName` | `Register` | `string` | `n` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:1307` | 寄存器主汇编名；共享寄存器模型建立名字表。 |
| `CostPerUse` | `Register` | `list<int>` | `[0]` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:166`, `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:668` | 每寄存器使用成本序列；共享寄存器模型消费。 |
| `CoveredBySubRegs` | `Register` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:167` | 值由子寄存器决定；共享寄存器模型消费。 |
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
| `CopyCost` | `RegisterClass` | `int` | `1` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:811` | 寄存器拷贝代价；8-bit，>255 fatal，负值实际按 uint8_t 截断。 |
| `GeneratePressureSet` | `RegisterClass` | `bit` | `true` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:758` | 是否生成寄存器压力集。 |
| `GlobalPriority` | `RegisterClass` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:823` | 全局分配优先级。 |
| `MemberList` | `RegisterClass` | `dag` | `regList` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:1249` | 不是普通 list：经 SetTheory field expander 展开，决定寄存器类成员集与默认分配序。 |
| `RegInfos` | `RegisterClass` | `RegInfoByHwMode` | `（未设默认值）` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:798` | 按 HwMode 的 `{RegSize,SpillSize,SpillAlignment}` 三元组；覆盖默认模式值。 |
| `RegTypes` | `RegisterClass` | `list<ValueType>` | `regTypes` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:759` | 寄存器类支持的 MVT 列表；`Size==0` 时用于推断位宽。 |
| `SpillStackID` | `RegisterClass` | `int` | `0` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:814` | spill stack slot 的自定义 ID；影响 frame index 分配。 |
| `TSFlags` | `RegisterClass`, `Instruction`, `SDNode` | `bits<8>` / `bits<64>` / `bits<32>` | `0` | `derive` ⚠ | `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:1932`, `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:825` | 目标自定义标志位：`Instruction`/`RegisterClass` 写入 MCInstrDesc/寄存器类；`SDNode.TSFlags` 是 DAG 节点标志，参与匹配。 |
| `Weight` | `RegisterClass` | `int` | `?` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:920` | 寄存器类的压力权重；影响压力集计算。 |
| `isAllocatable` | `RegisterClass` | `bit` | `true` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:812` | 是否可分配；决定 `InAllocatableClassTable`。 |
| `RegAsmNames` | `RegisterTuples` | `list<string>` | `RegNames` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:651` | `RegisterTuples` 展开时覆盖每个合成寄存器的 `AsmName`。 |
| `Classes` | `RegisterCategory` | `list<RegisterClass>` | `classes` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:1235` | `RegisterCategory` 包含的寄存器类。 |
| `AddedComplexity` | `Instruction`, `Pattern` | `int` | `0` | `derive` ⚠ | `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:4496` | pattern/指令的优先级加成；越大越先匹配。 |
| `AsmString` | `Instruction` | `string` | `""` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:420` | 汇编模板；`CodeGenInstruction` 保存，供 asm writer/matcher 使用；本后端只解析不直接发射。 |
| `CodeSize` | `Instruction` | `int` | `0` | `project` | `llvm/utils/TableGen/DAGISelEmitter.cpp:73` | 指令的 code size 成本；DAGISel 用它排序匹配候选。 |
| `Constraints` | `Instruction`, `SDTypeProfile` | `string` / `list<SDTypeConstraint>` | `""` / `constraints` | `derive` ⚠ | `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:1939`, `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:480` | `SDTypeProfile` 的类型约束列表。 |
| `Defs` | `Instruction` | `list<Register>` | `[]` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:472` | 隐式定义寄存器列表；合并进 `ImplicitDefs`，发射到 `ImplicitLists`。 |
| `FastISelShouldIgnore` | `Instruction` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:453` | FastISel 忽略该指令；只保存在 `CodeGenInstruction`，不进入 MCInstrDesc。 |
| `InOperandList` | `Instruction` | `dag` | `（未设默认值）` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:43` | 输入操作数 dag；展开成 use 列表，参与操作数编号与 `MIOperandInfo` 解析。 |
| `OutOperandList` | `Instruction` | `dag` | `（未设默认值）` | `derive` | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:29` | 输出操作数 dag；`CodeGenInstruction` 展开成 def 列表，决定 `NumDefs`。 |
| `Pattern` | `Instruction` | `list<dag>` | `（未设默认值）` | `derive` | `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:4138`, `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:4139` | 指令上的 pattern 列表；与独立 `Pattern` 记录一起进入匹配器。 |
| `Predicates` | `Instruction`, `Pattern` | `list<Predicate>` | `[]` | `derive` ⚠ | `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:4495`, `llvm/utils/TableGen/Common/CodeGenHwModes.cpp:27` | 指令谓词列表；`computeRequiredFeatures` 用它编码每个 opcode 的 feature bitset。 |
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
| `MIOperandInfo` | `Operand` | `dag` | `(ops)` | `derive` | `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:2501`, `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:2783`, `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:97` | 复合操作数的子操作数 dag；`CollectOperandInfo` 用它展开 MC operand 列表。 |
| `OperandType` | `Operand`, `RegisterOperand` | `string` | `"OPERAND_UNKNOWN"` / `"OPERAND_REGISTER"` | `derive` ⚠ | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:88`, `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:93` | MC operand 的 C++ 类型名；`CodeGenInstruction` 存入 `OperandType`。 |
| `PrintMethod` | `Operand`, `RegisterOperand` | `string` | `"printOperand"` / `pm` | `derive` ⚠ | `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:87`, `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:92` | 自定义打印函数名；`CodeGenInstruction` 保存，供 asm writer 调用。 |
| `Type` | `Operand` | `ValueType` | `ty` | `derive` | `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:1860`, `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:2452` | 操作数的 `ValueType`；类型推断时使用。 |
| `RegClass` | `RegisterOperand` | `RegisterClassLike` | `regclass` | `derive` | `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:1871`, `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:2358`, `llvm/utils/TableGen/Common/CodeGenTarget.cpp:274`, `llvm/utils/TableGen/Common/InfoByHwMode.cpp:224` | 操作数绑定的寄存器类；解析为 `RegisterClass` 并用于寄存器操作数信息。 |
| `DefaultOps` | `OperandWithDefaultOps` | `dag` | `defaultops` | `derive` | `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:3550` | `OperandWithDefaultOps` 的默认操作数 dag。 |
| `OperandNum` | `SDTypeConstraint` | `int` | `opnum` | `derive` | `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:1580` | `SDTypeConstraint` 约束的操作数编号。 |
| `OtherOperandNum` | `SDTCisSameAs`, `SDTCisVTSmallerThanOp`, `SDTCisSameNumEltsAs`, `SDTCisSameSizeAs` | `int` | `OtherOp` | `derive` ⚠ | `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:1598`, `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:1601`, `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:1625`, `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:1628` | 与 `OperandNum` 比较的另一个操作数编号。 |
| `BigOperandNum` | `SDTCisOpSmallerThanOp` | `int` | `BigOp` | `derive` | `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:1604` | `SDTCisOpSmallerThanOp` 中较大的操作数编号。 |
| `OtherOpNum` | `SDTCisEltOfVec`, `SDTCisSubVecOfVec` | `int` | `OtherOp` | `derive` ⚠ | `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:1607`, `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:1610` | `SDTCisEltOfVec`/`SDTCisSubVecOfVec` 的另一个操作数编号。 |
| `VT` | `SDTCVecEltisVT` | `ValueType` | `vt` | `derive` | `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:1584`, `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:1613`, `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:2639`, `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:2653` | 约束要求的 `ValueType`。 |
| `NumOperands` | `SDTypeProfile`, `ComplexPattern` | `int` | `numoperands` / `numops` | `derive` ⚠ | `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:1925`, `llvm/utils/TableGen/Common/CodeGenTarget.cpp:415` | select 函数返回的操作数个数；`SDTypeProfile` 中则是节点操作数个数。 |
| `NumResults` | `SDTypeProfile` | `int` | `numresults` | `derive` | `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:1924` | `SDTypeProfile` 的结果个数。 |
| `IsStrictFP` | `SDNode` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:1929` | 严格浮点节点标记；影响匹配与重排。 |
| `Opcode` | `SDNode`, `SDNodeXForm` | `string` / `SDNode` | `opcode` / `opc` | `derive` ⚠ | `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:1921`, `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:3444` | `SDNode` 的 opcode 字符串（`ISD::XXX`）；`SDNodeXForm` 中则是被变换的节点。 |
| `SDClass` | `SDNode` | `string` | `sdclass` | `derive` | `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:1922` | 生成的 C++ SDNode 子类名；决定 cast 的目标类型。 |
| `TypeProfile` | `SDNode` | `SDTypeProfile` | `typeprof` | `derive` | `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:1923` | `SDTypeProfile` 引用；给出结果/操作数个数与类型约束。 |
| `XFormFunction` | `SDNodeXForm` | `code` | `xformFunction` | `derive` | `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:3445` | `SDNodeXForm` 的 C++ 变换函数体。 |
| `AddressSpaces` | `PatFrags` | `list<int>` | `?` | `derive` | `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:1306` | 可接受的地址空间列表；空表示任意。 |
| `Fragments` | `PatFrags` | `list<dag>` | `frags` | `derive` | `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:1999`, `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:3469` | 候选 fragment 列表；结构匹配时任一命中即可。 |
| `GISelPredicateCode` | `PatFrags` | `code` | `[{}]` | `derive` | `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:1325`, `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:1330` | GlobalISel 等价谓词；导入 pattern 时使用。 |
| `ImmediateCode` | `PatFrags` | `code` | `[{}]` | `derive` | `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:1197`, `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:1201` | 立即数值检查的 C++ 片段；SDISel 与 GlobalISel 共享。 |
| `MemoryVT` | `PatFrags` | `ValueType` | `?` | `derive` | `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:1299` | load/store fragment 的内存值类型。 |
| `MinAlignment` | `PatFrags` | `int` | `?` | `derive` | `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:1313` | 内存 fragment 要求的最小对齐。 |
| `OperandTransform` | `PatFrags` | `SDNodeXForm` | `xform` | `derive` | `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:3514` | 绑定 `$name` 时套用的 `SDNodeXForm`。 |
| `Operands` | `PatFrags` | `dag` | `ops` | `derive` | `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:3484` | `PatFrags` 的命名嵌套操作数 dag；fragment 里用 `$name` 引用。 |
| `PredicateCode` | `PatFrags` | `code` | `pred` | `derive` | `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:919`, `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:1186` | 结构匹配后的 C++ 谓词；为空表示只看结构。 |
| `ScalarMemoryVT` | `PatFrags` | `ValueType` | `?` | `derive` | `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:1320` | 向量内存操作实际存储的标量类型。 |
| `IsAPFloat` | `ImmLeaf` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:1211` | `ImmLeaf` 的立即数是否按 `APFloat` 处理。 |
| `IsAPInt` | `ImmLeaf` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:1205` | `ImmLeaf` 的立即数是否按 `APInt` 处理。 |
| `GISelShouldIgnore` | `Pattern` | `bit` | `0` | `derive` | `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:4555` | 为 true 时 GlobalISel 导入忽略该 pattern。 |
| `PatternToMatch` | `Pattern` | `dag` | `patternToMatch` | `derive` | `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:4527` | pattern 的源 dag；`TreePattern` 从它构建匹配树。 |
| `ResultInstrs` | `Pattern` | `list<dag>` | `resultInstrs` | `derive` | `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:4535` | pattern 的结果指令列表；决定选中后生成哪些 MachineInstr。 |
| `Complexity` | `ComplexPattern` | `int` | `complexity` | `derive` | `llvm/utils/TableGen/Common/CodeGenTarget.cpp:423` | 复杂 pattern 的相对优先级；越大越先匹配。 |
| `Properties` | `ComplexPattern` | `list<SDNodeProperty>` | `props` | `derive` | `llvm/utils/TableGen/Basic/SDNodeProperties.cpp:18`, `llvm/utils/TableGen/Common/CodeGenTarget.cpp:432` | 结果节点需具备的 `SDNode` 属性；也用于校验 pattern 与指令一致。 |
| `RootNodes` | `ComplexPattern` | `list<SDNode>` | `roots` | `derive` | `llvm/utils/TableGen/Common/CodeGenTarget.cpp:417` | 复杂匹配可接受的根节点列表。 |
| `SelectFunc` | `ComplexPattern` | `string` | `fn` | `derive` | `llvm/utils/TableGen/Common/CodeGenTarget.cpp:416` | 执行复杂匹配的 C++ 函数名。 |
| `Ty` | `ComplexPattern` | `ValueType` | `ty` | `derive` | `llvm/utils/TableGen/Common/CodeGenTarget.cpp:414` | `ComplexPattern` 的返回值类型。 |
| `WantsParent` | `ComplexPattern` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenTarget.cpp:455` | 把父节点作为额外参数传给 select 函数。 |
| `WantsRoot` | `ComplexPattern` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenTarget.cpp:454` | 把匹配根节点作为额外参数传给 select 函数。 |
| `AssemblerCondDag` | `Predicate` | `dag` | `（未设默认值）` | `derive` | `llvm/utils/TableGen/Common/SubtargetFeatureInfo.cpp:181`, `llvm/utils/TableGen/Common/SubtargetFeatureInfo.cpp:203`, `llvm/utils/TableGen/Common/SubtargetFeatureInfo.cpp:224` | 汇编器谓词的 dag 条件。 |
| `AssemblerMatcherPredicate` | `Predicate` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/SubtargetFeatureInfo.cpp:32`, `llvm/utils/TableGen/Common/SubtargetFeatureInfo.cpp:169`, `llvm/utils/TableGen/Common/SubtargetFeatureInfo.cpp:220` | 是否作为汇编匹配谓词参与生成。 |
| `CondString` | `Predicate` | `code` | `cond` | `derive` | `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:1566`, `llvm/utils/TableGen/Common/SubtargetFeatureInfo.cpp:39`, `llvm/utils/TableGen/Common/SubtargetFeatureInfo.cpp:113`, `llvm/utils/TableGen/Common/SubtargetFeatureInfo.cpp:158` | 谓词的 C++ 条件字符串；pattern 把它拼进匹配代码。 |
| `RecomputePerFunction` | `Predicate` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/SubtargetFeatureInfo.h:51` | 是否每个函数重新计算该谓词。 |

`stage`：`schema` 仅声明 · `derive` 只被共享派生层读 · `project` 只被本 emitter 读 · `derive+project` 两者都读。

**声明了但本后端不读的字段**

| 字段 | 声明于 | 类型 | 归属 |
| --- | --- | --- | --- |
| `AltNames` | `Register` | `list<string>` | 未知（需查其它后端） |
| `DwarfNumbers` | `Register` | `list<int>` | 未知（需查其它后端） |
| `HWEncoding` | `Register` | `bits<16>` | 未知（需查其它后端） |
| `PositionOrder` | `Register`, `RegisterTuples` | `int` | 未知（需查其它后端） |
| `RegAltNameIndices` | `Register` | `list<RegAltNameIndex>` | 未知（需查其它后端） |
| `DecoderMethod` | `DAGOperand`, `InstructionEncoding` | `string` | decoder 自定义方法。 |
| `hasCompleteDecoder` | `DAGOperand`, `InstructionEncoding` | `bit` | decoder 完整性标记。 |
| `CoerceFromRegClass` | `RegisterClass` | `RegisterClass` | asm matcher 的寄存器类强制。 |
| `DiagnosticString` | `RegisterClass` | `string` | asm matcher 诊断文本。 |
| `DiagnosticType` | `RegisterClass` | `string` | asm matcher 诊断类型。 |
| `altNameIndex` | `RegisterClass` | `RegAltNameIndex` | 寄存器类别名索引；asm writer 消费。 |
| `DwarfAlias` | `DwarfRegAlias` | `Register` | DWARF 别名；`-gen-register-info` 消费。 |
| `DecoderNamespace` | `InstructionEncoding` | `string` | decoder 生成 namespace。 |
| `AsmMatchConverter` | `Instruction` | `string` | asm matcher 的自定义转换器。 |
| `AsmVariantName` | `Instruction` | `string` | asm variant 选择。 |
| `EncodingInfos` | `Instruction` | `EncodingByHwMode` | 按 HwMode 的编码；encoder/decoder 消费。 |
| `GISelMatchGenericTypes` | `Instruction` | `bit` | GlobalISel 泛型类型匹配。 |
| `HasPositionOrder` | `Instruction` | `bit` | asm matcher 的位置排序。 |
| `Itinerary` | `Instruction` | `InstrItinClass` | 旧调度模型；`-gen-instr-info` 不读。 |
| `PostEncoderMethod` | `Instruction` | `string` | 编码后处理函数。 |
| `SchedRW` | `Instruction` | `list<SchedReadWrite>` | 新调度模型读写资源。 |
| `TwoOperandAliasConstraint` | `Instruction` | `string` | 两操作数 alias 约束；asm matcher 消费。 |
| `UseLogicalOperandMappings` | `Instruction` | `bit` | 未知（需查其它后端） |
| `UseNamedOperandTable` | `Instruction` | `bit` | 未知（需查其它后端） |
| `isAsmParserOnly` | `Instruction` | `bit` | 仅汇编器使用，codegen 不可见。 |
| `supportsDeactivationSymbol` | `Instruction` | `bit` | 指令级 deactivation 标记；其它后端消费。 |
| `MCOperandPredicate` | `Operand` | `code` | MC operand 谓词；asm writer 消费。 |
| `ParserMatchClass` | `Operand`, `RegisterOperand` | `AsmOperandClass` | asm parser 匹配类。 |
| `GIZeroRegister` | `RegisterOperand` | `Register` | GlobalISel 零寄存器。 |
| `HasNoUse` | `PatFrags` | `bit` | 预置谓词标记；`PredicateExpanderDag` 生成检查，不在本 source set。 |
| `HasOneUse` | `PatFrags` | `bit` | 预置谓词标记；`PredicateExpanderDag` 生成检查，不在本 source set。 |
| `IsAnyExtLoad` | `PatFrags` | `bit` | 预置加载谓词；`PredicateExpanderDag` 生成检查。 |
| `IsAtomic` | `PatFrags` | `bit` | 预置内存谓词；`PredicateExpanderDag` 生成检查。 |
| `IsAtomicOrderingAcquire` | `PatFrags` | `bit` | 预置原子序谓词；`PredicateExpanderDag` 生成检查。 |
| `IsAtomicOrderingAcquireOrStronger` | `PatFrags` | `bit` | 预置原子序谓词；`PredicateExpanderDag` 生成检查。 |
| `IsAtomicOrderingAcquireRelease` | `PatFrags` | `bit` | 预置原子序谓词；`PredicateExpanderDag` 生成检查。 |
| `IsAtomicOrderingMonotonic` | `PatFrags` | `bit` | 预置原子序谓词；`PredicateExpanderDag` 生成检查。 |
| `IsAtomicOrderingRelease` | `PatFrags` | `bit` | 预置原子序谓词；`PredicateExpanderDag` 生成检查。 |
| `IsAtomicOrderingReleaseOrStronger` | `PatFrags` | `bit` | 预置原子序谓词；`PredicateExpanderDag` 生成检查。 |
| `IsAtomicOrderingSequentiallyConsistent` | `PatFrags` | `bit` | 预置原子序谓词；`PredicateExpanderDag` 生成检查。 |
| `IsLoad` | `PatFrags` | `bit` | 预置内存谓词；`PredicateExpanderDag` 生成检查。 |
| `IsNonExtLoad` | `PatFrags` | `bit` | 预置加载谓词；`PredicateExpanderDag` 生成检查。 |
| `IsSignExtLoad` | `PatFrags` | `bit` | 预置加载谓词；`PredicateExpanderDag` 生成检查。 |
| `IsStore` | `PatFrags` | `bit` | 预置内存谓词；`PredicateExpanderDag` 生成检查。 |
| `IsTruncStore` | `PatFrags` | `bit` | 预置存储谓词；`PredicateExpanderDag` 生成检查。 |
| `IsUnindexed` | `PatFrags` | `bit` | 预置内存谓词；`PredicateExpanderDag` 生成检查。 |
| `IsZeroExtLoad` | `PatFrags` | `bit` | 预置加载谓词；`PredicateExpanderDag` 生成检查。 |
| `PredicateCodeUsesOperands` | `PatFrags` | `bit` | 允许 `PredicateCode` 引用 `Operands`；匹配器生成阶段消费。 |
| `FastIselShouldIgnore` | `ImmLeaf` | `bit` | fast-isel 的过滤开关；dag-isel 不读它。 |
| `PredicateName` | `Predicate` | `string` | `getSubtargetFeatureName` 生成用；`-gen-subtarget`/asm matcher 消费。 |

**名字即 API**（不经过字段查找的硬编码名字）

| 名字 | 出现位置 | 作用 |
| --- | --- | --- |
| `set` | `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:1977`, `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:3125`, `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:3657` …(+1) | pattern 结果绑定节点；分隔匹配树与结果树。 |
| `node` | `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:2180`, `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:2443`, `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:3017` …(+1) | 未类型化叶子的占位节点。 |
| `srcvalue` | `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:2443`, `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:3607` | 源值叶子；表示已加载的值。 |
| `null_frag` | `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:3885`, `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:3890` | 空 fragment；使 pattern 失效。 |
| `immAllOnesV / immAllZerosV` | `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:1479`, `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:2444`, `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:2445` | 全 1/全 0 向量立即数；类型推断特殊处理。 |
| `INSERT_SUBREG / REG_SEQUENCE` | `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:2707`, `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:2711` | 结果树里的寄存器合成伪节点；决定操作数映射。 |

<!-- END: GENERATED DSL CONTRACT -->

<!-- BEGIN: GENERATED PROJECTION MATRIX -->

只列“本后端之外还有读者”或“同名多声明”的字段；`shared` 表示共享派生层也读它。

| 字段 | 声明于 | shared | 其它 `-gen-*` | helper | 同名 |
| --- | --- | --- | --- | --- | --- |
| `Objects` | `RegInfoByHwMode`, `SubRegRangeByHwMode`, `RegClassByHwMode` | ✓ | `instr-info` | — | ⚠ |
| `Offset` | `SubRegRange`, `SubRegIndex` | ✓ | — | — | ⚠ |
| `Size` | `SubRegRange`, `SubRegIndex`, `RegisterClass`, `Instruction` | ✓ | `asm-matcher`, `callingconv`, `compress-inst-emitter`, `instr-info` | `helper:DecoderEmitter`, `helper:X86RecognizableInstr` | ⚠ |
| `Namespace` | `SubRegIndex`, `Register`, `RegisterClass`, `Instruction` | ✓ | `asm-matcher`, `asm-writer`, `emitter`, `fast-isel`, `pseudo-lowering`, `register-info`, `subtarget` | — | ⚠ |
| `AltNames` | `Register` | — | `asm-matcher`, `asm-writer` | — | — |
| `AsmName` | `Register` | ✓ | `asm-matcher`, `asm-writer` | — | — |
| `DwarfNumbers` | `Register` | — | `register-info` | — | — |
| `HWEncoding` | `Register` | — | `register-info` | — | — |
| `PositionOrder` | `Register`, `RegisterTuples` | — | — | — | ⚠ |
| `RegAltNameIndices` | `Register` | — | `asm-matcher`, `asm-writer` | — | — |
| `SubRegIndices` | `Register`, `RegisterTuples` | ✓ | — | — | ⚠ |
| `SubRegs` | `Register`, `RegisterTuples` | ✓ | — | — | ⚠ |
| `DecoderMethod` | `DAGOperand`, `InstructionEncoding` | ✓ | — | — | ⚠ |
| `hasCompleteDecoder` | `DAGOperand`, `InstructionEncoding` | ✓ | — | — | ⚠ |
| `Alignment` | `RegisterClass` | ✓ | — | `helper:X86RecognizableInstr` | — |
| `CoerceFromRegClass` | `RegisterClass` | — | `asm-matcher` | — | — |
| `DiagnosticString` | `RegisterClass` | — | `asm-matcher` | — | — |
| `DiagnosticType` | `RegisterClass` | — | `asm-matcher` | — | — |
| `TSFlags` | `RegisterClass`, `Instruction`, `SDNode` | ✓ | `instr-info` | — | ⚠ |
| `DwarfAlias` | `DwarfRegAlias` | — | `register-info` | — | — |
| `DecoderNamespace` | `InstructionEncoding` | ✓ | — | `helper:DecoderEmitter` | — |
| `AddedComplexity` | `Instruction`, `Pattern` | ✓ | — | — | ⚠ |
| `AsmMatchConverter` | `Instruction` | — | `asm-matcher` | — | — |
| `AsmString` | `Instruction` | ✓ | `insn-directive-match-table` | — | — |
| `AsmVariantName` | `Instruction` | — | `asm-matcher`, `x86-mnemonic-tables` | — | — |
| `Constraints` | `Instruction`, `SDTypeProfile` | ✓ | `instr-docs` | — | ⚠ |
| `Defs` | `Instruction` | ✓ | `global-isel-combiner`, `x86-fold-tables`, `x86-instr-mapping` | — | — |
| `EncodingInfos` | `Instruction` | ✓ | `emitter` | `helper:DecoderEmitter` | — |
| `GISelMatchGenericTypes` | `Instruction` | — | `global-isel` | — | — |
| `HasPositionOrder` | `Instruction` | — | `asm-matcher` | — | — |
| `InOperandList` | `Instruction` | ✓ | `insn-directive-match-table`, `x86-fold-tables` | — | — |
| `OutOperandList` | `Instruction` | ✓ | `insn-directive-match-table`, `x86-fold-tables` | — | — |
| `PostEncoderMethod` | `Instruction` | ✓ | `emitter` | — | — |
| `Predicates` | `Instruction`, `Pattern` | ✓ | `asm-matcher`, `asm-writer`, `compress-inst-emitter`, `global-isel-combiner`, `instr-docs`, `instr-info`, `macro-fusion-pred`, `subtarget`, `x86-instr-mapping` | `helper:DecoderEmitter`, `helper:X86RecognizableInstr` | ⚠ |
| `TwoOperandAliasConstraint` | `Instruction` | — | `asm-matcher` | — | — |
| `UseLogicalOperandMappings` | `Instruction` | — | `instr-info` | — | — |
| `UseNamedOperandTable` | `Instruction` | — | `instr-info` | — | — |
| `Uses` | `Instruction` | ✓ | `x86-fold-tables` | — | — |
| `isAsmParserOnly` | `Instruction` | — | `x86-fold-tables`, `x86-instr-mapping` | `helper:DecoderEmitter`, `helper:X86RecognizableInstr` | — |
| `isCodeGenOnly` | `Instruction` | ✓ | — | `helper:DecoderEmitter`, `helper:X86RecognizableInstr` | — |
| `isPseudo` | `Instruction` | ✓ | `searchable-tables`, `x86-fold-tables` | — | — |
| `supportsDeactivationSymbol` | `Instruction` | — | — | `helper:DAGISelMatcherEmitter` | — |
| `EncoderMethod` | `Operand`, `RegisterOperand` | ✓ | — | — | ⚠ |
| `MCOperandPredicate` | `Operand` | — | `asm-writer` | — | — |
| `MIOperandInfo` | `Operand` | ✓ | `instr-info` | `helper:DAGISelMatcherGen` | — |
| `OperandType` | `Operand`, `RegisterOperand` | ✓ | `asm-writer`, `global-isel-combiner` | `helper:X86RecognizableInstr` | ⚠ |
| `ParserMatchClass` | `Operand`, `RegisterOperand` | — | `asm-matcher`, `insn-directive-match-table` | — | ⚠ |
| `PrintMethod` | `Operand`, `RegisterOperand` | ✓ | `asm-writer` | — | ⚠ |
| `Type` | `Operand` | ✓ | `global-isel-combiner`, `x86-fold-tables`, `x86-instr-mapping` | — | — |
| `GIZeroRegister` | `RegisterOperand` | — | `global-isel` | — | — |
| `RegClass` | `RegisterOperand` | ✓ | `asm-matcher`, `fast-isel`, `global-isel`, `instr-info` | `helper:DAGISelMatcherGen`, `helper:X86RecognizableInstr` | — |
| `OtherOperandNum` | `SDTCisSameAs`, `SDTCisVTSmallerThanOp`, `SDTCisSameNumEltsAs`, `SDTCisSameSizeAs` | ✓ | — | — | ⚠ |
| `OtherOpNum` | `SDTCisEltOfVec`, `SDTCisSubVecOfVec` | ✓ | — | — | ⚠ |
| `NumOperands` | `SDTypeProfile`, `ComplexPattern` | ✓ | — | — | ⚠ |
| `Opcode` | `SDNode`, `SDNodeXForm` | ✓ | `global-isel`, `x86-fold-tables`, `x86-instr-mapping` | — | ⚠ |
| `GISelPredicateCode` | `PatFrags` | ✓ | `global-isel` | — | — |
| `ImmediateCode` | `PatFrags` | ✓ | `global-isel` | — | — |
| `FastIselShouldIgnore` | `ImmLeaf` | — | `fast-isel` | — | — |
| `IsAPFloat` | `ImmLeaf` | ✓ | `global-isel` | — | — |
| `IsAPInt` | `ImmLeaf` | ✓ | `global-isel` | — | — |
| `AssemblerCondDag` | `Predicate` | ✓ | `asm-writer` | — | — |
| `AssemblerMatcherPredicate` | `Predicate` | ✓ | `asm-writer`, `compress-inst-emitter` | `helper:DecoderEmitter` | — |
| `CondString` | `Predicate` | ✓ | `global-isel`, `global-isel-combiner`, `x86-instr-mapping` | — | — |
| `PredicateName` | `Predicate` | — | `asm-matcher` | — | — |

矩阵覆盖 64 个字段（共 186 个）。
<!-- END: GENERATED PROJECTION MATRIX -->

<!-- BEGIN: GENERATED DERIVATIONS -->

Record 里只有声明；这些是共享 C++ 在投影之前对语义的改写。

| 改写 | Schema 键 | 证据 | 说明 |
| --- | --- | --- | --- |
| `pattern collection` | `Pattern` | `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:4526` | 收集独立 `Pattern` 并展开成 `TreePattern`。 |
| `instruction pattern merge` | `Instruction.Pattern` | `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:4139` | 把指令上的 `Pattern` 列表并入匹配器。 |
| `fragment flattening` | `PatFrags.Fragments` | `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:1999`, `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:3469` | 把候选 fragment 展开成可匹配的树。 |
| `type inference` | `SDNode.TypeProfile` | `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:357`, `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:375` …(+16) | 按类型约束推断每个节点/操作数的 `ValueType`。 |
| `predicate lowering` | `Predicate.CondString` | `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:1566`, `llvm/utils/TableGen/Common/SubtargetFeatureInfo.cpp:39` …(+2) | 把谓词条件拼进匹配树。 |
| `node registration` | `SDNode` | `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:3430` | 注册 `SDNode` 的 opcode、属性与类型 profile。 |
| `operand type resolution` | `Operand.Type` | `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:1860`, `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:2452` | 把操作数解析到 `ValueType`。 |
<!-- END: GENERATED DERIVATIONS -->

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
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:123` | `hasSideEffects` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:150` | `hasSideEffects` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:185` | `hasSideEffects` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:196` | `isTerminator` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:207` | `isBranch` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:236` | `hasSideEffects` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:255` | `hasSideEffects` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:459` | `Predicates` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:512` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:513` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:514` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:515` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:516` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:518` | `TSFlags` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:527` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:528` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:529` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:530` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:531` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:532` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:534` | `TSFlags` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:544` | `Predicates` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:610` | `Predicates` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:675` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:676` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:677` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:678` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:679` | `TSFlags` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:680` | `mayLoad` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:681` | `mayStore` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:687` | `OutOperandList` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:688` | `InOperandList` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:689` | `AsmString` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:690` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:691` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:692` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:700` | `OutOperandList` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:701` | `InOperandList` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:702` | `AsmString` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:703` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:704` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:705` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:722` | `Predicates` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:741` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:742` | `hasSideEffects` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:745` | `Predicates` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:788` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:789` | `isBarrier` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:793` | `Inst` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:803` | `isReturn` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:804` | `isBarrier` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:805` | `isTerminator` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:806` | `Size` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:814` | `isBranch` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:815` | `isBarrier` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:816` | `isTerminator` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:817` | `Size` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:828` | `Defs` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:852` | `isCall` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:861` | `isCall` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:883` | `isTerminator` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:891` | `isTerminator` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:903` | `isBranch` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:904` | `isBarrier` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:905` | `isTerminator` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:909` | `isIndirectBranch` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinInstrInfo.td:910` | `Size` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:12` | `FallbackRegAltNameIndex` | 不在本后端契约（其它后端消费或拼写错误） |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:17` | `Namespace` | 消费（derive） |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:18` | `HWEncoding` | 本后端不读 |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:19` | `AltNames` | 本后端不读 |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:20` | `DwarfNumbers` | 本后端不读 |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:25` | `RegAltNameIndices` | 本后端不读 |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:16` | `Num` | 自定义字段（需自行保证 name-based 读取方存在） |

共 96 处使用；契约外 28 处。
<!-- END: GENERATED TD USAGE -->

RVMin 的 pattern 都写在 `RVMinInstrInfo.td`（`def : Pat<...>`），而
`--usage` 只识别字段赋值行，所以生成块里的 96 处使用来自指令/寄存器属性，
不是 pattern 本身。pattern 的影响面要看契约的 `Pattern` 根与
`PatternToMatch` / `ResultInstrs` 字段。

## 构建与验证

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop
T="$W/llvm/utils/TableGen/dsl-surface.py"
DOC="$W/llvm/lib/Target/RVMin/docs/36-dag-isel-dsl.md"

# 1. 直接看契约
python3 "$T" --backend dag-isel
python3 "$T" --backend dag-isel --matrix

# 2. 文档块更新/校验
python3 "$T" --backend dag-isel --update "$DOC" \
  --usage "$W/llvm/lib/Target/RVMin/RVMinInstrInfo.td" \
          "$W/llvm/lib/Target/RVMin/RVMinRegisterInfo.td"
python3 "$T" --backend dag-isel --check "$DOC" \
  --usage "$W/llvm/lib/Target/RVMin/RVMinInstrInfo.td" \
          "$W/llvm/lib/Target/RVMin/RVMinRegisterInfo.td"

# 3. 六后端矩阵与字段影响面
python3 "$T" --status-matrix --usage \
  "$W/llvm/lib/Target/RVMin/RVMinRegisterInfo.td" \
  "$W/llvm/lib/Target/RVMin/RVMin.td"
python3 "$T" --effect PatternToMatch

# 4. 全量门禁
"$W/llvm/lib/Target/RVMin/scripts/rvmin-verify-all.sh" "$B/bin"
```

## 预期输出

```text
$ python3 dsl-surface.py --backend dag-isel
dag-isel: {'roots': 14, 'fields': 186, 'consumed': 136,
           'unused': 50, 'stages': {'schema': 50, 'derive': 135,
           'project': 1, 'derive+project': 0}}

$ python3 dsl-surface.py --backend dag-isel --check doc36.md --usage ...
dsl-surface: OK (dag-isel: 136/186 fields consumed
              [135 derive, 1 project, 0 both], 6 magic names, 7 derivations)

$ python3 dsl-surface.py --status-matrix --usage ...
dsl-surface: OK (cross-backend status: 13 usages, 6 backends)

$ scripts/rvmin-verify-all.sh "$B/bin"
...
PASS 7/7: TableGen DSL contracts + status matrix + field effect
ALL GATES PASS (run rvmin-run.sh separately for the QEMU check)
```

## 常见坑

1. **emitter 薄不等于契约小。** `DAGISelEmitter.cpp` 只有 1 个读取点，
   但 source set 里的共享分析器贡献了 135 个 `derive` 字段。
2. **`derive+project` 为 0 是正常的。** 不要以为每个后端都有“两边都读”
   的字段；dag-isel 的 emitter 把工作完全委托给分析器。
3. **`--usage` 看不到 `def : Pat<...>`。** 它扫描字段赋值，不扫描记录定义；
   pattern 的影响面用契约根和 `--matrix` 回答。
4. **helper class 不是可选的。** `SDTypeProfile` / `SDTC*` / `ValueType` /
   `Predicate` 都没有根枚举，不列进 `helper_classes` 就不会出现在 schema 里。
5. **不要把 `CodeGenIntrinsics.cpp` 加进 source set。** 它会把 29 个
   `Intrinsic` 字段带进来，而 dag-isel 真正关心的是 pattern 树。
6. **magic name 可能在共享代码。** 第 36 章已把扫描范围扩到整个 source set；
   只扫 emitter 会漏掉 `set` / `node` / `null_frag`。
7. **186 个字段里 50 个是 schema。** 不是所有 `Instruction` 字段都参与
   dag 选择；schema 是“可读但本后端不读”的完整声明面。
8. **派生类字段不会自动进入 schema。** `collect_fields()` 只沿基类展开；
   `ImmLeaf.FastIselShouldIgnore` 这类字段必须把派生类加进
   `helper_classes`，否则注解和矩阵都会静默漏掉。

## 练习

1. 加 `-gen-fast-isel`，比较它的 `project` 数与 dag-isel 的 1。
2. 把 `CodeGenIntrinsics.cpp` 临时加进 `sources`，看字段数变化，再解释为什么
   最终排除。
3. 给 `--usage` 增加 `def : Pat<...>` 检测：它应该输出 pattern 名、
   `PatternToMatch` 的根节点，还是整棵树？
4. 用 `--effect PatternToMatch` 看六个后端的投影，解释为什么只有
   dag-isel 消费它。
5. 用 `--effect TSFlags` 对比第 33 章：新增的 dag-isel 列是 `derive` 还是
   `—`？为什么？

## 产物清单

| 文件 | 变更 |
| --- | --- |
| `llvm/utils/TableGen/dsl-surface.py` | 新增 `dag-isel` Backend；56 个注解；`ImmLeaf` helper；magic 扫描扩到 source set |
| `llvm/lib/Target/RVMin/docs/36-dag-isel-dsl.md` | 新增：本章（四个生成块） |
| `llvm/lib/Target/RVMin/docs/32-cross-backend-status.md` | 生成块升级为六后端 |
| `llvm/lib/Target/RVMin/docs/33-field-effect.md` | 生成块升级为六后端 |
| `llvm/lib/Target/RVMin/docs/34-asm-matcher-dsl.md` | 联动说明改为六后端 |
| `llvm/lib/Target/RVMin/docs/README.md` | 新增第 36 章索引行；范围改为 29–36（第 37 章扩到 29–37） |
| `llvm/lib/Target/RVMin/scripts/rvmin-verify-all.sh` | 第 7 项增加 dag-isel 校验 |
