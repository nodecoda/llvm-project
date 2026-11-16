# 第 37 章：第六个后端——`-gen-fast-isel` 与派生类 schema

## 目标

第 36 章的 dag-isel 给出一个很干净的投影：186 个字段里 **135 个 derive、
1 个 project、0 个 derive+project**。本章加第六个后端 `-gen-fast-isel`。
它和 dag-isel 共享几乎全部 source set，却给出另一组数字：

| 后端 | roots | 字段 | 消费 | derive / project / both | schema | magic | derivations |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `register-info` | 8 | 59 | 44 | 39 / 4 / 1 | 15 | 10 | 11 |
| `instr-info` | 6 | 111 | 85 | 74 / 5 / 6 | 26 | 0 | 8 |
| `asm-writer` | 8 | 130 | 99 | 84 / 9 / 6 | 31 | 2 | 8 |
| `asm-matcher` | 15 | 166 | 142 | 95 / 42 / 5 | 24 | 2 | 7 |
| `dag-isel` | 14 | 186 | 136 | 135 / 1 / 0 | 50 | 6 | 7 |
| `fast-isel` | **14** | **186** | **136** | **133 / 1 / 2** | **50** | **0** | **4** |

要验证的问题：

> 两个后端共享同一份 schema、同一套共享分析器，契约能不能把
> **emitter 的边界**单独显示出来？

答案是能，而且差异正好落在三个桶里：

- `derive` 从 135 降到 133：有 2 个字段变成“共享层和 emitter 都读”；
- `project` 仍是 1，但字段从 `CodeSize` 换成 `FastIselShouldIgnore`；
- `schema` 仍是 50，`roots` 仍是 14，字段总数仍是 186。

本章产出：

- `dsl-surface.py` 增加 `--backend fast-isel`；
- 契约：14 个记录类入口、186 个字段、136 个被消费、64 行投影矩阵、
  4 条派生改写、0 个硬编码名字；
- 注解在 dag-isel 基础上只多 1 个：`FastIselShouldIgnore`；
- 关键修复：`ImmLeaf` 是**派生类**，必须显式列进 `helper_classes`，
  否则它的字段会静默消失（这次修复也回头改了第 36 章的数字）；
- 第 32 章 status 矩阵与第 33 章 `--effect` 自动变成六后端。

非目标：

- 不修改 `FastISelEmitter` / `CodeGenDAGPatterns` 的行为；
- 不给 RVMin 实现 C++ 的 FastISel 类——本章只分析 TableGen 投影，
  `-gen-fast-isel` 生成的通用查表代码并不等于目标接入了 fast-isel；
- 不覆盖 `-gen-disassembler`。

## 背景：为什么 fast-isel 是第六个

dag-isel 和 fast-isel 是同一个问题的两种投影：

```text
.td records
   │
   ▼
CodeGenDAGPatterns  ──┬─→ DAGISelEmitter   （完整匹配器：树 → 状态机）
   │                  └─→ FastISelEmitter  （简单 pattern：树 → 查表）
   ▼
共享派生层（寄存器模型、指令模型、类型推断、谓词）
```

`DAGISelEmitter.cpp` 只读 1 个 schema 字段（`CodeSize`），几乎把所有工作
交给共享分析器。`FastISelEmitter.cpp` 不一样：它自己要做一轮**能力过滤**，
只保留“看起来足够简单”的 pattern，然后把结果发射成按类型/操作符索引的
lookup 表。

过滤规则全部写在 C++ 里，`.td` 里没有“fast-isel 合法”这个字段：

- 结果树必须是一条 `Instruction`，不能是多指令 pattern；
- 结果必须定义在 operand 0（`EXTRACT_SUBREG` 是特例）；
- 源树的操作数必须同 VT；立即数只允许一个 immediate predicate，
  且该谓词不能引用 `Operands`；
- 寄存器操作数必须能解析到 `RegisterClass`；
- 其它非叶子节点一律拒绝；
- 指令可以用 `Instruction.FastISelShouldIgnore` 整体跳过；
- 立即数谓词的 `PatFrag` 可以用 `ImmLeaf.FastIselShouldIgnore` 跳过。

这张清单是**过程式的合法性**，不是 schema 字段。这正是本章最有教学价值的
地方：record 描述了完整的**声明式** schema，但“某个后端会不会用它”是
投影函数，必须从 emitter 反推。

## 代码地图

| 文件 | 作用 |
| --- | --- |
| `llvm/utils/TableGen/FastISelEmitter.cpp` | 本后端 emitter：过滤 + 发射 lookup 表 |
| `llvm/utils/TableGen/Common/CodeGenDAGPatterns.{h,cpp}` | `Pattern` / `TreePattern` / 类型推断，与 dag-isel 共用 |
| `llvm/utils/TableGen/Common/CodeGenInstruction.{h,cpp}` | `Instruction.FastISelShouldIgnore` 缓存进 `CodeGenInstruction` |
| `llvm/utils/TableGen/Common/CodeGenRegisters.{h,cpp}` | 寄存器类与 `RegClass` 解析 |
| `llvm/utils/TableGen/Common/CodeGenTarget.{h,cpp}` | 指令/寄存器查询 |
| `llvm/utils/TableGen/Common/InfoByHwMode.{h,cpp}` | HwMode 相关寄存器类信息 |
| `llvm/include/llvm/Target/Target.td` | `Instruction.FastISelShouldIgnore`（大写 S，第 813 行） |
| `llvm/include/llvm/Target/TargetSelectionDAG.td` | `ImmLeaf`（第 1300 行）及其 `FastIselShouldIgnore`（小写 s） |
| `llvm/utils/TableGen/dsl-surface.py` | **本章改造**：第六个 Backend + `ImmLeaf` helper |
| `llvm/lib/Target/RVMin/docs/37-fast-isel-dsl.md` | 本章；四个生成块由脚本生成 |

## 关键实现：第六个后端怎么接进来

### 1. `sources` / `roots_from`：与 dag-isel 同构，只换 emitter

```python
"fast-isel": Backend(
    name="fast-isel",
    emitter="llvm/utils/TableGen/FastISelEmitter.cpp",
    sources=(
        ("llvm/utils/TableGen/FastISelEmitter.cpp", "emitter"),
        # 其余 16 个文件与 dag-isel 完全相同：
        # CodeGenDAGPatterns / CodeGenInstruction / CodeGenRegisters /
        # CodeGenTarget / CodeGenHwModes / InfoByHwMode /
        # SubtargetFeatureInfo / SDNodeProperties
    ),
    td_files=(
        "llvm/include/llvm/Target/Target.td",
        "llvm/include/llvm/Target/TargetSelectionDAG.td",
        "llvm/include/llvm/Target/TargetSubtarget.td",
    ),
    roots_from=(
        "llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp",
        "llvm/utils/TableGen/Common/CodeGenRegisters.cpp",
    ),
    extra_roots=(
        "Instruction", "Register", "RegisterClass",
        "Operand", "RegisterOperand",
    ),
    helper_classes=(
        # 与 dag-isel 相同的 helper，外加 ImmLeaf（见下一节）
        "ImmLeaf",
    ),
)
```

`roots_from` 决定哪些文件参与 `getAllDerivedDefinitions()` 根发现；
`sources` 决定读取点扫描范围。两者都不含 `DAGISelEmitter.cpp`，所以
fast-isel 的契约里不会出现 dag-isel 的 `CodeSize`。

### 2. `helper_classes` 的边界：`ImmLeaf` 是派生类

`collect_fields()` 只沿**基类**展开。`ImmLeaf` 继承自 `PatFrags`，
它的三个字段声明在派生类上：

| 字段 | 声明于 | fast-isel 是否读 |
| --- | --- | --- |
| `IsAPInt` | `ImmLeaf` | 否（`global-isel` 读） |
| `IsAPFloat` | `ImmLeaf` | 否（`global-isel` 读） |
| `FastIselShouldIgnore` | `ImmLeaf` | 是 |

如果只把 `PatFrags` 放进 helper，这三个字段根本不会进入 schema。
本次把 `ImmLeaf` 同时加进 **dag-isel 和 fast-isel** 的 helper 列表，
于是 dag-isel 的数字也发生变化：

| 指标 | 加 `ImmLeaf` 前 | 加 `ImmLeaf` 后 |
| --- | --- | --- |
| 字段 | 183 | **186** |
| 消费 | 134 | **136** |
| schema | 49 | **50** |
| 矩阵行 | 61 | **64** |

> 这是契约工具的一个通用教训：**“沿基类展开”不是 schema 闭包**。
> 派生类字段要么通过 helper 显式引入，要么永远不可见，而且不会有任何
> 报错——字段消失后，缺注解检查也一起消失。

### 3. 注解：fast-isel 只多 1 个

`FAST_ISEL_ANNOTATIONS` 直接复制 dag-isel 的 56 个新注解，再把
`FastIselShouldIgnore` 从“未消费注记”提升为“已消费注解”：

```python
FAST_ISEL_ANNOTATIONS = dict(DAG_ISEL_ANNOTATIONS)
FAST_ISEL_ANNOTATIONS.update({
    "FastIselShouldIgnore": "为 true 时 fast-isel 跳过该立即数谓词的 pattern，不进入 lookup 表。",
})
```

注意 dag-isel 仍然把同名（小写 s）字段放进 `DAG_ISEL_UNUSED_NOTES`，
注记是“fast-isel 的过滤开关；dag-isel 不读它”。同一个字段在两个后端里
一个 `schema`、一个 `project`，注解文本也随之切换。

### 4. 派生改写：4 条

fast-isel 只保留与它实际路径相关的 4 条：

- pattern filtering：`ImmLeaf.FastIselShouldIgnore`；
- pattern collection：`Pattern` → `TreePattern`（与 dag-isel 共用）；
- type inference：`SDNode.TypeProfile`；
- operand type resolution：`Operand.RegClass`。

dag-isel 的 `Instruction.Pattern` 合并、`PatFrags.Fragments` 展开、
`Predicate.CondString` 下沉、`SDNode` 注册等条目在这里不出现，因为
fast-isel 不走完整的 matcher 构造路径——它只消费已经构建好的
`PatternToMatch`。

### 5. magic name：0

fast-isel 没有直接比较 dag 操作符名字的代码，它通过共享的
`TreePatternNode` API 取 operator/children。因此“名字即 API”一栏为空。

这不是说 fast-isel 更简单，而是说它的**名字契约**完全落在共享层里：
`set` / `node` / `null_frag` 这些名字由 `CodeGenDAGPatterns.cpp` 处理，
已经在第 36 章的 6 个 magic name 中覆盖。

### 6. 投影特征：133 derive / 1 project / 2 both

与 dag-isel 的差异只有 4 个字段，但每一个都能讲清 emitter 边界：

| 字段 | 声明于 | dag-isel | fast-isel | 原因 |
| --- | --- | --- | --- | --- |
| `CodeSize` | `Instruction` | `project` | `schema` | 只有 `DAGISelEmitter.cpp` 读它 |
| `FastIselShouldIgnore` | `ImmLeaf` | `schema`（未消费） | `project` | `FastISelEmitter.cpp:224` 直接读原记录 |
| `Namespace` | `SubRegIndex` / `Register` / `RegisterClass` / `Instruction` | `derive` | `derive+project` | `FastISelEmitter.cpp:422` 用它拼物理寄存器全名 |
| `RegClass` | `RegisterOperand` | `derive` | `derive+project` | `FastISelEmitter.cpp:266,475` 解析操作数寄存器类 |

> **易混点：两个只差大小写的字段。**
> `Instruction.FastISelShouldIgnore`（大写 S）是 `Target.td` 里的指令级开关，
> 由 `CodeGenInstruction.cpp:453` 缓存，emitter 通过 `Inst.FastISelShouldIgnore`
> 使用，stage 是 `derive`；
> `ImmLeaf.FastIselShouldIgnore`（小写 s）是 `TargetSelectionDAG.td` 里
> 立即数谓词 fragment 的开关，emitter 在第 224 行直接
> `getValueAsBit("FastIselShouldIgnore")`，stage 是 `project`。
> `grep -i` 会把两者混在一起；契约用 `(declaring class, field)` 把它们分开。

`project` 仍然只有 1 个，并不代表 emitter 简单：`FastISelEmitter.cpp`
有上千行过滤与发射逻辑，但它读取的**schema 字段**很少——大量信息来自
`TreePatternNode` 的方法返回值，而不是 record 字段。契约度量的是后者。

## 契约与投影

下面四段由脚本生成，不要手改；改 emitter 或 `.td` 后运行 `--update`。

<!-- BEGIN: GENERATED DSL CONTRACT -->

后端：`None` · emitter：`llvm/utils/TableGen/FastISelEmitter.cpp`
统计：14 个记录类入口 · 186 个字段（schema 50 · derive 133 · project 1 · derive+project 2）。

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
| `Namespace` | `SubRegIndex`, `Register`, `RegisterClass`, `Instruction` | `string` | `""` / `namespace` | `derive+project` ⚠ | `llvm/utils/TableGen/FastISelEmitter.cpp:422`, `llvm/utils/TableGen/Common/CodeGenInstruction.cpp:419`, `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:58`, `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:59` …(+4) | 指令/寄存器命名空间；`Target.getInstNamespace()` 决定生成 enum 的 C++ namespace。 |
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
| `RegClass` | `RegisterOperand` | `RegisterClassLike` | `regclass` | `derive+project` | `llvm/utils/TableGen/FastISelEmitter.cpp:266`, `llvm/utils/TableGen/FastISelEmitter.cpp:475`, `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:1871`, `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:2358` …(+2) | 操作数绑定的寄存器类；解析为 `RegisterClass` 并用于寄存器操作数信息。 |
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
| `FastIselShouldIgnore` | `ImmLeaf` | `bit` | `false` | `project` | `llvm/utils/TableGen/FastISelEmitter.cpp:224` | 为 true 时 fast-isel 跳过该立即数谓词的 pattern，不进入 lookup 表。 |
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
| `CodeSize` | `Instruction` | `int` | isel 的 code size 排序；`-gen-dag-isel` 消费。 |
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
| `PredicateName` | `Predicate` | `string` | `getSubtargetFeatureName` 生成用；`-gen-subtarget`/asm matcher 消费。 |

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
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:17` | `Namespace` | 消费（derive+project） |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:18` | `HWEncoding` | 本后端不读 |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:19` | `AltNames` | 本后端不读 |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:20` | `DwarfNumbers` | 本后端不读 |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:25` | `RegAltNameIndices` | 本后端不读 |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:16` | `Num` | 自定义字段（需自行保证 name-based 读取方存在） |

共 96 处使用；契约外 28 处。
<!-- END: GENERATED TD USAGE -->

RVMin 没有 `def : Pat<...>` 之外的 fast-isel 专属标记；生成块里的 96 处
使用仍来自指令/寄存器属性。`--usage` 无法回答“哪个 pattern 会被
fast-isel 接受”，那要看上面的过滤规则。

<!-- BEGIN: GENERATED PROJECTION MATRIX -->

只列“本后端之外还有读者”或“同名多声明”的字段；`shared` 表示共享派生层也读它。

| 字段 | 声明于 | shared | 其它 `-gen-*` | helper | 同名 |
| --- | --- | --- | --- | --- | --- |
| `Objects` | `RegInfoByHwMode`, `SubRegRangeByHwMode`, `RegClassByHwMode` | ✓ | `instr-info` | — | ⚠ |
| `Offset` | `SubRegRange`, `SubRegIndex` | ✓ | — | — | ⚠ |
| `Size` | `SubRegRange`, `SubRegIndex`, `RegisterClass`, `Instruction` | ✓ | `asm-matcher`, `callingconv`, `compress-inst-emitter`, `instr-info` | `helper:DecoderEmitter`, `helper:X86RecognizableInstr` | ⚠ |
| `Namespace` | `SubRegIndex`, `Register`, `RegisterClass`, `Instruction` | ✓ | `asm-matcher`, `asm-writer`, `emitter`, `pseudo-lowering`, `register-info`, `subtarget` | — | ⚠ |
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
| `CodeSize` | `Instruction` | — | `dag-isel` | — | — |
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
| `RegClass` | `RegisterOperand` | ✓ | `asm-matcher`, `global-isel`, `instr-info` | `helper:DAGISelMatcherGen`, `helper:X86RecognizableInstr` | — |
| `OtherOperandNum` | `SDTCisSameAs`, `SDTCisVTSmallerThanOp`, `SDTCisSameNumEltsAs`, `SDTCisSameSizeAs` | ✓ | — | — | ⚠ |
| `OtherOpNum` | `SDTCisEltOfVec`, `SDTCisSubVecOfVec` | ✓ | — | — | ⚠ |
| `NumOperands` | `SDTypeProfile`, `ComplexPattern` | ✓ | — | — | ⚠ |
| `Opcode` | `SDNode`, `SDNodeXForm` | ✓ | `global-isel`, `x86-fold-tables`, `x86-instr-mapping` | — | ⚠ |
| `GISelPredicateCode` | `PatFrags` | ✓ | `global-isel` | — | — |
| `ImmediateCode` | `PatFrags` | ✓ | `global-isel` | — | — |
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
| `pattern filtering` | `ImmLeaf.FastIselShouldIgnore` | `llvm/utils/TableGen/FastISelEmitter.cpp:224` | 跳过显式标记的立即数谓词 fragment，只把简单 pattern 收进 lookup 表。 |
| `pattern collection` | `Pattern` | `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:4526` | 与 dag-isel 共用同一套 `TreePattern` 构建。 |
| `type inference` | `SDNode.TypeProfile` | `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:357`, `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:375` …(+16) | 按类型约束推断节点/操作数的 `ValueType`。 |
| `operand type resolution` | `Operand.RegClass` | `llvm/utils/TableGen/FastISelEmitter.cpp:266`, `llvm/utils/TableGen/FastISelEmitter.cpp:475` …(+4) | 把操作数解析到具体 `RegisterClass`。 |
<!-- END: GENERATED DERIVATIONS -->

## 构建与验证

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop
T="$W/llvm/utils/TableGen/dsl-surface.py"
DOC="$W/llvm/lib/Target/RVMin/docs/37-fast-isel-dsl.md"

# 1. 直接看契约
python3 "$T" --backend fast-isel
python3 "$T" --backend fast-isel --matrix

# 2. 文档块更新/校验
python3 "$T" --backend fast-isel --update "$DOC" \
  --usage "$W/llvm/lib/Target/RVMin/RVMinInstrInfo.td" \
          "$W/llvm/lib/Target/RVMin/RVMinRegisterInfo.td"
python3 "$T" --backend fast-isel --check "$DOC" \
  --usage "$W/llvm/lib/Target/RVMin/RVMinInstrInfo.td" \
          "$W/llvm/lib/Target/RVMin/RVMinRegisterInfo.td"

# 3. 六后端矩阵与字段影响面
python3 "$T" --status-matrix --usage \
  "$W/llvm/lib/Target/RVMin/RVMinRegisterInfo.td" \
  "$W/llvm/lib/Target/RVMin/RVMin.td"
python3 "$T" --effect RegClass

# 4. 全量门禁
"$W/llvm/lib/Target/RVMin/scripts/rvmin-verify-all.sh" "$B/bin"
```

## 预期输出

```text
$ python3 dsl-surface.py --backend fast-isel
fast-isel: {'roots': 14, 'fields': 186, 'consumed': 136,
            'unused': 50, 'stages': {'schema': 50, 'derive': 133,
            'project': 1, 'derive+project': 2}}

$ python3 dsl-surface.py --backend fast-isel --check doc37.md --usage ...
dsl-surface: OK (fast-isel: 136/186 fields consumed
              [133 derive, 1 project, 2 both], 0 magic names, 4 derivations)

$ python3 dsl-surface.py --status-matrix --usage ...
dsl-surface: OK (cross-backend status: 13 usages, 6 backends)

$ scripts/rvmin-verify-all.sh "$B/bin"
...
PASS 7/7: TableGen DSL contracts + status matrix + field effect
ALL GATES PASS (run rvmin-run.sh separately for the QEMU check)
```

## 常见坑

1. **派生类字段不会自动进入 schema。** `ImmLeaf` 必须显式列进
   `helper_classes`；否则 `FastIselShouldIgnore` / `IsAPInt` / `IsAPFloat`
   会静默消失，`--check` 也不会报错。
2. **同名字段大小写不同。** `FastISelShouldIgnore`（`Instruction`）和
   `FastIselShouldIgnore`（`ImmLeaf`）是两个契约条目，不是一个。
3. **fast-isel 的合法性不在 `.td` 里。** 过滤规则是 C++ 过程式判断；
   `--usage` 只能看到字段赋值，看不到“这个 pattern 是否足够简单”。
4. **`project` 少不代表 emitter 小。** 它只统计直接读取的 schema 字段；
   大量逻辑走 `TreePatternNode` API，不落在字段读取上。
5. **magic name 为 0 不代表没有名字契约。** 名字处理都在共享分析器里，
   由 dag-isel 的 6 个 magic name 覆盖。
6. **`-gen-fast-isel` ≠ RVMin 支持 fast-isel。** 本章生成的是通用 lookup
   表；RVMin 并没有 C++ `FastISel` 子类，代码生成仍走 SelectionDAG。
7. **schema 里混着别的后端字段。** `IsAPInt` / `IsAPFloat` 是
   `global-isel` 的输入，fast-isel 只是“看得到、不读”。

## 练习

1. 把 `ImmLeaf` 从 `helper_classes` 移除，观察字段数、矩阵行和注解检查
   分别怎么变；解释为什么 `--check` 不会发现字段丢失。
2. 用 `--effect RegClass` 对比 dag-isel 和 fast-isel 的列，指出是哪两个
   读取点把 `derive` 变成 `derive+project`。
3. 用 `--effect CodeSize` 和 `--effect FastIselShouldIgnore` 对比：一个
   从 `project` 退回 `schema`，一个从 `schema` 升为 `project`。这说明
   `project` 是“后端 × 字段”的函数。
4. 给 `--usage` 增加一个“fast-isel 可接受性”分析：需要读取哪些
   `TreePatternNode` 方法？哪些信息根本不在字段里？
5. 如果把 `DAGISelEmitter.cpp` 加进 fast-isel 的 `sources`，会发生什么？
   先预测 `CodeSize` 的 stage，再运行验证。

## 产物清单

| 文件 | 变更 |
| --- | --- |
| `llvm/utils/TableGen/dsl-surface.py` | 新增 `fast-isel` Backend；57 个注解；`ImmLeaf` 进 helper（同时影响 dag-isel） |
| `llvm/lib/Target/RVMin/docs/37-fast-isel-dsl.md` | 新增：本章（四个生成块） |
| `llvm/lib/Target/RVMin/docs/36-dag-isel-dsl.md` | `ImmLeaf` 修复后的数字与注解表；六后端联动 |
| `llvm/lib/Target/RVMin/docs/32-cross-backend-status.md` | 生成块升级为六后端 |
| `llvm/lib/Target/RVMin/docs/33-field-effect.md` | 生成块升级为六后端 |
| `llvm/lib/Target/RVMin/docs/34-asm-matcher-dsl.md` | 联动说明改为六后端 |
| `llvm/lib/Target/RVMin/docs/35-upstream-contract.md` | 练习里的后端数改为六 |
| `llvm/lib/Target/RVMin/docs/README.md` | 新增第 37 章索引行；范围改为 29–37 |
| `llvm/lib/Target/RVMin/scripts/rvmin-verify-all.sh` | 第 7 项增加 fast-isel 校验 |
| `llvm/docs/TableGen/BackEnds.md` | 后端列表加 `-gen-fast-isel` |
