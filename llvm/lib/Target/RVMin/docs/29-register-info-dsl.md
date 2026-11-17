# 第 29 章：把 TableGen 后端的 DSL 语义公开化（实验：`-gen-register-info`）

## 目标

本章不加指令、不改代码生成。它解决写 `.td` 时最难受的一件事：

> 我们在 `.td` 里写的“关键字”，有些是 `-gen-*` 后端的字段，有些是变量/常量，
> 有些只是给别的后端用的。哪些真正生效、默认值是什么、越界会怎样，
> 没有一份确切公开的说明；最终只能复制粘贴。

本章把这层语义从 emitter 源码里**反推**出来，并做成两件可执行的东西：

1. 一份机器可读的契约：记录类入口、字段、默认值、读取点、是否被本后端消费；
2. 一个漂移门禁：emitter 或 `.td` 一改，契约不同步就失败，避免文档再次腐烂。

实验对象是 `-gen-register-info`——它足够小（emitter 2253 行），又足够典型
（8 个记录类、59 个字段、10 个硬编码名字、21 个跨后名字段、11 条派生改写）。

非目标：

- 不修改 `RegisterInfoEmitter` 的任何行为；
- 不做完整的 C++ 语义分析（声明归属按“声明类”聚合，读取点仍按“字段名”，
  两者的区别见“背景”与“常见坑”）；
- 不覆盖其它 43 个 `-gen-*` 后端；但投影矩阵会扫描它们，提取器也按后端
  配置化，扩展点见“练习”。

## 背景：四层模型，最后一层完全不可见

先修正一个容易含糊的模型。一个 `.td` 字段从输入到产物，要经过四层：

| 层 | 形态 | 例子 | 可观察性 |
| --- | --- | --- | --- |
| 1. 语法 / 声明 | `.td` 文本 | `let Size = 64` | 源码 |
| 2. 完整声明模型 | `RecordKeeper` 里的 `Record` | class 默认值 + def 覆盖后的 `RecordVal` | `-print-records` |
| 3. 派生模型 | 共享 C++ 展开后的内存模型 | SetTheory 展开、tuple 合成、inferred class、排序 | 只能读 C++ |
| 4. 投影 | 某个 `-gen-*` 的输出 | `RegClassInfos[]`、`RegEncodingTable[]` | 产物 |

所以更准确的说法是：**Record 是完整的声明模型，`-gen-*` 是投影；两者之间
还隔着一层共享的派生模型。** 本章的契约因此不是一张字段清单，而是三张表：

1. **schema 表**：主键 `(declaring class, field)`，带类型、默认值、`stage`；
2. **投影矩阵**：同一个字段名还被哪些 `-gen-*` 读；
3. **派生改写表**：共享 C++ 在投影之前对 Record 做了什么。

只有把第 3 层显式化，才能解释为什么 `.td` 里写了 `let Size = 64`，
`-gen-register-info` 实际看到的值却可能来自 `RegInfos` 或推断。

### 三层“关键字”及其公开程度

`llvm-tblgen -gen-register-info` 的输入表面还可以按“表面积形态”拆成三层：

| 层 | `.td` 里的写法 | emitter 里的对应物 | 是否公开 |
| --- | --- | --- | --- |
| 记录类入口 | `def X0 : RVMinReg<...>`（继承链最终是 `Register`） | `getAllDerivedDefinitions("Register")` | 部分（`Target.td` 有注释） |
| 字段 | `let Namespace = "RVMin"`、`let TSFlags = 1` | `R->getValueAsInt("TSFlags")` | 分散，默认值只在 C++ 里 |
| 名字 | `def GeneralPurposeRegisters : RegisterCategory<...>` | `Category.getName() == "GeneralPurposeRegisters"` | **完全不可见** |

第三层最危险：它既不是字段，也没有类型检查。把 `GeneralPurposeRegisters`
拼成 `GPRs`，`llvm-tblgen` 不会报错，生成的
`isGeneralPurposeRegister()` 会永远返回 `false`。

还有一个容易走偏的地方：**C++ 侧按字段名读取，schema 侧按声明类归属**。
`R->getValueAsInt("Size")` 不关心 `R` 是 `RegisterClass`、`SubRegIndex`
还是 `SubRegRange`；只要记录上有 `Size` 就能读。但 `Size` 在这三个类里
语义不同（spill size / 子寄存器位宽 / 范围位宽）。所以契约主键必须是
`(declaring class, field)`，而读取点仍按字段名聚合——**两者都要，缺一不可**。
只按名字会把无关 schema 混进来，只按类会漏掉跨类共享的读取点。

### 为什么只看 `RegisterInfoEmitter.cpp` 不够

emitter 自己的字面量字段读取只有 8 处（5 个字段：`Namespace`、`DwarfNumbers`、
`DwarfAlias`、`HWEncoding`、`OtherPreserved`）；44 个被消费字段里有 39 个的
读取点在共享层。真正的大头在这里：

| 文件 | 角色 | 典型读取 |
| --- | --- | --- |
| `RegisterInfoEmitter.cpp` | emitter | `DwarfNumbers`、`HWEncoding`、`Namespace` |
| `Common/CodeGenRegisters.cpp` | 寄存器模型 | `CostPerUse`、`AltOrders`、`TSFlags`、`SpillStackID`… |
| `Common/InfoByHwMode.cpp` | HwMode 选择器 | `RegSize`/`SpillSize`/`SpillAlignment`、`Size`/`Offset` |
| `Common/CodeGenRegisters.h` | 内联访问器 | `BaseClassOrder` |
| `include/llvm/TableGen/Record.h` | 核心排序器 | `PositionOrder`（间接决定枚举顺序） |
| `CodeGenRegisters.cpp` 的 `addFieldExpander` | SetTheory | `MemberList`、`SaveList`（不是 `getValue*`） |

只 grep emitter 会漏掉近 90% 的被消费字段（39/44），也会把 `PositionOrder`
这种“在核心层生效”的字段误判为死字段。

## 代码地图

| 文件 | 作用 |
| --- | --- |
| `llvm/utils/TableGen/RegisterInfoEmitter.cpp` | 本后端 emitter；`run()` 返回 5 个产物 |
| `llvm/utils/TableGen/Common/CodeGenRegisters.{h,cpp}` | 寄存器/寄存器类/子寄存器索引模型 |
| `llvm/utils/TableGen/Common/InfoByHwMode.{h,cpp}` | `{Modes,Objects}` 选择器 |
| `llvm/include/llvm/Target/Target.td` | `Register`/`RegisterClass`/`SubRegIndex`/… 的字段声明与默认值 |
| `llvm/include/llvm/Target/TargetCallingConv.td` | `CalleeSavedRegs` |
| `llvm/utils/TableGen/dsl-surface.py` | **本章新增**：契约提取器 + 漂移检查 |
| `llvm/lib/Target/RVMin/docs/29-register-info-dsl.md` | 本章；四个 `GENERATED` 块由脚本生成 |

## 关键实现：从 emitter 反推契约

### 六步流水线

`dsl-surface.py` 不解析 C++ AST，而是用“受控的词法扫描 + `.td` 轻量解析”：

1. **记录类入口**：只在前两个文件里找 `getAllDerivedDefinitions("X")`
   （`roots_from` 限定）。`RegAltNameIndex` 不经过这个接口，用
   `Target.getRegAltNameIndices()` 显式补一个根，并验证钩子仍存在。
2. **声明模型**：按大括号配对解析 `class`/`def`，抽取字段声明、默认值、
   继承列表，并保留真正的**声明类**——这是 `(declaring class, field)` 主键的
   来源；`RegisterByHwMode` 会把 `HwModeSelect`、`RegisterOperand`
   的继承字段一并带进来。
3. **派生模型**：`DERIVATIONS` 表列出投影之前的改写（SetTheory、tuple、
   推断、排序、HwMode 绑定），每条正则必须在共享源码里命中，否则报错。
4. **读取点**：在 emitter 闭包的文件里匹配
   `getValue*("Name")` / `getValueInit("Name")`。支持跨行调用；先剥掉注释，
   避免把注释里的字段名算进去。读取点按 `emitter` / `shared` / `core`
   标注，并换算成 `stage`。
5. **投影矩阵**：扫描 `llvm/utils/TableGen/**/*.{cpp,h}`，把文件里的
   `"gen-xxx"` 字面量当作 reader；`Common/`、`Basic/` 归为 `shared`，
   没有 gen 字面量的归为 `helper:<Stem>`。这样能看到一个字段名跨后端的
   真实影响面。
6. **名字契约与未消费字段**：把 `getName() == "..."` 这类硬编码名字单独
   成表；同时列出“声明了但本后端不读”的字段，并标注归属后端。

### 产物

- `--json`：完整契约（每个读取点的 `文件:行`、访问器、代码片段、scope，
  以及 `declared_in` / `reached_from` / `stage` / `ambiguous` / `projects`）；
- `--print-block`：只打印文档里的机器契约块；
- `--matrix`：只打印跨后端投影矩阵，快速看“改这个字段还会影响谁”；
- `--usage a.td b.td`：把目标 `.td` 的 `let`/`field` 用法对照契约；
- `--update DOC --usage ...`：重写文档中的四个生成块；
- `--check DOC --usage ...`：校验四个生成块未漂移 + **每个被消费字段都必须
  有人写语义** + **每条派生改写仍然命中**。

最后一条是本章的核心约束：**新增一个被读取的字段，却不在 `ANNOTATIONS` 里
写清语义，`--check` 直接失败**。这样“关键字”就不可能再悄悄变成 hardcode。

### 机器契约

下面这一段由脚本生成，不要手改；改 emitter 后运行 `--update`。

<!-- BEGIN: GENERATED DSL CONTRACT -->

后端：`gen-register-info` · emitter：`llvm/utils/TableGen/RegisterInfoEmitter.cpp`
CLI：`-register-info-debug`
产物（MultiFile）：`Enums.inc`, `MCDesc.inc`, `Header.inc`, `TargetDesc.inc`，主文件为 `<Target>GenRegisterInfo.inc`。
统计：8 个记录类入口 · 59 个字段（schema 15 · derive 39 · project 4 · derive+project 1）。

**记录类入口**

| 记录类 | 定义 | 消费证据 |
| --- | --- | --- |
| `SubRegIndex` | `llvm/include/llvm/Target/Target.td:108` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:1256` |
| `RegAltNameIndex` | `llvm/include/llvm/Target/Target.td:162` | `llvm/utils/TableGen/RegisterInfoEmitter.cpp:179` |
| `Register` | `llvm/include/llvm/Target/Target.td:173` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:1264` |
| `RegisterClass` | `llvm/include/llvm/Target/Target.td:292` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:1354` |
| `RegisterTuples` | `llvm/include/llvm/Target/Target.td:478` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:1271`, `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:1288` |
| `RegisterCategory` | `llvm/include/llvm/Target/Target.td:506` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:1393` |
| `RegisterByHwMode` | `llvm/include/llvm/Target/Target.td:1086` | `llvm/utils/TableGen/RegisterInfoEmitter.cpp:220`, `llvm/utils/TableGen/RegisterInfoEmitter.cpp:1427` |
| `CalleeSavedRegs` | `llvm/include/llvm/Target/TargetCallingConv.td:235` | `llvm/utils/TableGen/RegisterInfoEmitter.cpp:1985` |

**字段契约**（主键是 `(declaring class, field)`；`stage` 表示它停在哪一层，`⚠` 表示同名多声明）

| 字段 | 声明于 | 类型 | 默认值 | stage | 本后端读取点 | 语义 |
| --- | --- | --- | --- | --- | --- | --- |
| `Modes` | `HwModeSelect` | `list<HwMode>` | `Ms` | `derive` | `llvm/utils/TableGen/Common/CodeGenHwModes.cpp:40` | 与 `Objects` 等长（.td 断言），按 ModeId 绑定。 |
| `RegSize` | `RegInfo` | `int` | `RS` | `derive` | `llvm/utils/TableGen/Common/InfoByHwMode.cpp:105` | `RegInfo`：寄存器位数。 |
| `SpillAlignment` | `RegInfo` | `int` | `SA` | `derive` | `llvm/utils/TableGen/Common/InfoByHwMode.cpp:107` | `RegInfo`：spill slot 对齐。 |
| `SpillSize` | `RegInfo` | `int` | `SS` | `derive` | `llvm/utils/TableGen/Common/InfoByHwMode.cpp:106` | `RegInfo`：spill slot 位数。 |
| `Objects` | `RegInfoByHwMode`, `SubRegRangeByHwMode`, `RegisterByHwMode` | `list<RegInfo>` / `list<SubRegRange>` / `list<Register>` | `Ts` / `Registers` | `derive` ⚠ | `llvm/utils/TableGen/Common/CodeGenHwModes.cpp:41`, `llvm/utils/TableGen/Common/CodeGenTarget.cpp:119` | n 号对象绑定 n 号 mode；对 `RegisterByHwMode` 是寄存器列表，对 `RegInfoByHwMode`/`SubRegRangeByHwMode` 是参数列表。 |
| `Offset` | `SubRegRange`, `SubRegIndex` | `int` | `offset` | `derive` ⚠ | `llvm/utils/TableGen/Common/InfoByHwMode.cpp:189` | 子寄存器起始位；`-1` 表示不固定（如 tuple 元素）。 |
| `Size` | `SubRegRange`, `SubRegIndex`, `RegisterClass` | `int` | `size` / `0` | `derive` ⚠ | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:800`, `llvm/utils/TableGen/Common/InfoByHwMode.cpp:188` | 寄存器类：spill size，`0` = 按首个 `RegTypes` 推断；子寄存器索引/范围：位数。 |
| `ComposedOf` | `SubRegIndex` | `list<SubRegIndex>` | `[]` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:86` | 两个索引的复合关系；必须恰好 2 个，否则 fatal。 |
| `CoveringSubRegIndices` | `SubRegIndex` | `list<SubRegIndex>` | `[]` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:99` | 声明“拼接覆盖”关系；至少 2 个，否则 fatal（通常留空让 TableGen 推断）。 |
| `Namespace` | `SubRegIndex`, `RegAltNameIndex`, `Register`, `RegisterClass` | `string` | `""` / `namespace` | `derive+project` ⚠ | `llvm/utils/TableGen/RegisterInfoEmitter.cpp:143`, `llvm/utils/TableGen/RegisterInfoEmitter.cpp:460`, `llvm/utils/TableGen/RegisterInfoEmitter.cpp:578`, `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:58` …(+5) | 本后端据此生成 C++ namespace；取第一个 Register / RegisterClass / SubRegIndex 的值。 |
| `SubRegRanges` | `SubRegIndex` | `SubRegRangeByHwMode` | `（未设默认值）` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:61` | 按 HwMode 的 {Size,Offset}；未包含 DefaultMode 时回落到 `Size`/`Offset`。 |
| `Aliases` | `Register` | `list<Register>` | `[]` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:193` | 显式别名；对称化成别名图，参与 regunit / `getSubRegs()` / lane mask 计算。 |
| `AsmName` | `Register` | `string` | `n` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:1307` | 汇编名；用作 `RegistersByName` 去重 key。`RegisterTuples` 未给 `RegAsmNames` 时为空串。 |
| `CostPerUse` | `Register` | `list<int>` | `[0]` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:166`, `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:668` | 每寄存器成本序列；按全局最大长度补 0，写入 `CostPerUseTable`。 |
| `CoveredBySubRegs` | `Register` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:167` | 值完全由子寄存器决定；触发 covered-register 计算与合成子寄存器索引。 |
| `DwarfNumbers` | `Register` | `list<int>` | `[]` | `project` | `llvm/utils/TableGen/RegisterInfoEmitter.cpp:446`, `llvm/utils/TableGen/RegisterInfoEmitter.cpp:572` | 每个 flavour 一个 gcc/gdb 编号；`-1` 表示无映射。长度取全局最大值，短的补 `-1`。 |
| `HWEncoding` | `Register` | `bits<16>` | `0` | `project` | `llvm/utils/TableGen/RegisterInfoEmitter.cpp:1397` | bits<16>，转成整数写入 `RegEncodingTable[]`。 |
| `PositionOrder` | `Register`, `RegisterTuples` | `int` | `0` | `derive` ⚠ | `llvm/include/llvm/TableGen/Record.h:2239`, `llvm/include/llvm/TableGen/Record.h:2240` | 间接消费：`LessRecordRegister`（Record.h）先按它排序，决定寄存器枚举顺序与 tuple 内部顺序。 |
| `SubRegIndices` | `Register`, `RegisterTuples` | `list<SubRegIndex>` | `[]` / `Indices` | `derive` ⚠ | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:175`, `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:632` | 与 `SubRegs` 一一对应；`RegisterTuples` 展开时取自 tuple 定义。 |
| `SubRegs` | `Register`, `RegisterTuples` | `list<Register>` / `list<dag>` | `[]` / `Regs` | `derive` ⚠ | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:176`, `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:634` | 子寄存器列表；与 `SubRegIndices` 交给 `zip_equal` 配对。`RegisterTuples` 展开时被替换成 tuple。 |
| `isArtificial` | `Register` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:170` | 合成寄存器标记；写入 `MCRegisterDesc`，并参与寄存器类的 `Artificial` 计算（全 artificial 的类压力权重上限为 0）。 |
| `isConstant` | `Register` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:168` | 生成 `isConstantPhysReg()`，并自动加入所有 `CalleeSavedRegs` 的 mask。 |
| `Alignment` | `RegisterClass` | `int` | `alignment` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:807` | spill 对齐（bit）。 |
| `AllocationPriority` | `RegisterClass` | `int` | `0` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:818` | 0..31，越界 fatal；写入寄存器类表。 |
| `AltOrderSelect` | `RegisterClass` | `code` | `[{}]` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:813` | `code` 原文拼进生成的 `AltOrderSelect(MF, Rev)` 函数体；非空才生成 `getRawAllocationOrder()`。 |
| `AltOrders` | `RegisterClass` | `list<dag>` | `[]` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:764` | 额外分配序；每个 dag 用 SetTheory 求值，成员必须属于 `MemberList`，否则 fatal。 |
| `BaseClassOrder` | `RegisterClass` | `int` | `?` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.h:619` | `?` 时不参与；否则决定寄存器的“基类”归属与 `getPhysRegBaseClass()` 生成。 |
| `CopyCost` | `RegisterClass` | `int` | `1` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:811` | 8-bit 拷贝代价；>255 则 fatal。负值：注释说饱和为 255，实际被构造函数末尾的赋值覆盖，按 `uint8_t` 截断（见第 29 章 §深读 9）。 |
| `GeneratePressureSet` | `RegisterClass` | `bit` | `true` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:758` | `false` 时不为该类（及其派生类）生成寄存器压力集。 |
| `GlobalPriority` | `RegisterClass` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:823` | 写入寄存器类表，交给 greedy 分配器的全局启发式。 |
| `MemberList` | `RegisterClass` | `dag` | `regList` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:1249` | 不是普通 list：经 SetTheory field expander 展开（add/sub/sequence…），决定成员集与默认分配序。 |
| `RegInfos` | `RegisterClass` | `RegInfoByHwMode` | `（未设默认值）` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:798` | 按 HwMode 的 {RegSize,SpillSize,SpillAlignment}；非空时覆盖 `Size`/`Alignment` 的默认模式值。 |
| `RegTypes` | `RegisterClass` | `list<ValueType>` | `regTypes` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:759` | 非空（.td 断言）；决定 `RegClassInfos` 的 MVT 列表，并在 `Size==0` 时提供推断位宽。 |
| `SpillStackID` | `RegisterClass` | `int` | `0` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:814` | 0..255，越界 fatal；写入寄存器类表。 |
| `TSFlags` | `RegisterClass` | `bits<8>` | `0` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:825` | bits<8>，原样写入寄存器类表。 |
| `Weight` | `RegisterClass` | `int` | `?` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:920` | `?` 时按 regunit 数计算；否则覆盖 `getRegClassWeight()` 的每寄存器权重。 |
| `isAllocatable` | `RegisterClass` | `bit` | `true` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:812` | 是否进入 `InAllocatableClassTable`，并决定哪些寄存器算“可分配”。 |
| `RegAsmNames` | `RegisterTuples` | `list<string>` | `RegNames` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:651` | tuple 寄存器的 asm 名列表；数量不足时 fatal。 |
| `Classes` | `RegisterCategory` | `list<RegisterClass>` | `classes` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:1235` | 类别包含的寄存器类。注意：本后端只识别三个 def 名，见“名字即 API”。 |
| `DwarfAlias` | `DwarfRegAlias` | `Register` | `reg` | `project` | `llvm/utils/TableGen/RegisterInfoEmitter.cpp:507` | 复制另一个寄存器的 `DwarfNumbers`（由 `DwarfRegAlias` 提供）；别名必须存在，否则 assert。 |
| `RegClass` | `RegisterOperand` | `RegisterClassLike` | `regclass` | `derive` | `llvm/utils/TableGen/Common/CodeGenTarget.cpp:274`, `llvm/utils/TableGen/Common/InfoByHwMode.cpp:224` | `RegisterByHwMode` 的目标寄存器类；每个 `Objects` 成员必须属于它（否则 fatal）。 |
| `RegistersAreIntervals` | `Target` | `bit` | `false` | `derive` | `llvm/utils/TableGen/Common/CodeGenTarget.cpp:157` | `Target` 记录上的开关；决定是否生成 `RegUnitIntervals` 表。 |
| `OtherPreserved` | `CalleeSavedRegs` | `dag` | `（未设默认值）` | `project` | `llvm/utils/TableGen/RegisterInfoEmitter.cpp:2002` | 只进 `<name>_RegMask`、不进 `_SaveList`；未设置时跳过。 |
| `SaveList` | `CalleeSavedRegs` | `dag` | `saves` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:1250` | 经 SetTheory field expander 展开；生成 `<name>_SaveList`，顺序即 prologue 保存顺序。 |

`stage`：`schema` 仅声明 · `derive` 只被共享派生层读 · `project` 只被本 emitter 读 · `derive+project` 两者都读。

**声明了但本后端不读的字段**

| 字段 | 声明于 | 类型 | 归属 |
| --- | --- | --- | --- |
| `FallbackRegAltNameIndex` | `RegAltNameIndex` | `RegAltNameIndex` | asm writer |
| `AltNames` | `Register` | `list<string>` | asm writer / asm matcher |
| `RegAltNameIndices` | `Register` | `list<RegAltNameIndex>` | asm writer / asm matcher |
| `DecoderMethod` | `DAGOperand` | `string` | decoder |
| `OperandNamespace` | `DAGOperand` | `string` | asm matcher / MC operand |
| `hasCompleteDecoder` | `DAGOperand` | `bit` | decoder |
| `CoerceFromRegClass` | `RegisterClass` | `RegisterClass` | asm matcher |
| `DiagnosticString` | `RegisterClass` | `string` | asm matcher |
| `DiagnosticType` | `RegisterClass` | `string` | asm matcher |
| `altNameIndex` | `RegisterClass` | `RegAltNameIndex` | asm writer |
| `EncoderMethod` | `RegisterOperand` | `string` | code emitter |
| `GIZeroRegister` | `RegisterOperand` | `Register` | isel / regalloc（generic zero register） |
| `OperandType` | `RegisterOperand` | `string` | MC operand |
| `ParserMatchClass` | `RegisterOperand` | `AsmOperandClass` | asm parser |
| `PrintMethod` | `RegisterOperand` | `string` | asm writer |

**名字即 API**（不经过字段查找的硬编码名字）

| 名字 | 出现位置 | 作用 |
| --- | --- | --- |
| `GeneralPurposeRegisters` | `llvm/utils/TableGen/RegisterInfoEmitter.cpp:2042`, `llvm/utils/TableGen/RegisterInfoEmitter.cpp:2056` | `RegisterCategory` 的 def 名；生成 `isGeneralPurposeRegister()/isGeneralPurposeRegisterClass()`。 |
| `FixedRegisters` | `llvm/utils/TableGen/RegisterInfoEmitter.cpp:2070` | `RegisterCategory` 的 def 名；生成 `isFixedRegister()`。 |
| `ArgumentRegisters` | `llvm/utils/TableGen/RegisterInfoEmitter.cpp:2084` | `RegisterCategory` 的 def 名；生成 `isArgumentRegister()`。 |
| `NoRegister` | `llvm/utils/TableGen/RegisterInfoEmitter.cpp:155`, `llvm/utils/TableGen/RegisterInfoEmitter.cpp:1884`, `llvm/utils/TableGen/RegisterInfoEmitter.cpp:1919` | 生成的枚举 0；`MCRegisterDesc` / 各表的第一项。 |
| `NUM_TARGET_REGS` | `llvm/utils/TableGen/RegisterInfoEmitter.cpp:161` | 生成枚举的哨兵，值为寄存器数 + 1。 |
| `NoSubRegister` | `llvm/utils/TableGen/RegisterInfoEmitter.cpp:196` | 子寄存器索引枚举 0；`composeSubRegIndices` 的空结果。 |
| `NUM_TARGET_SUBREGS` | `llvm/utils/TableGen/RegisterInfoEmitter.cpp:200` | 子寄存器索引枚举哨兵。 |
| `<name>_SaveList` | `llvm/utils/TableGen/RegisterInfoEmitter.cpp:1991` | `CalleeSavedRegs` def 名派生的保存列表。 |
| `<name>_RegMask` | `llvm/utils/TableGen/RegisterInfoEmitter.cpp:2016`, `llvm/utils/TableGen/RegisterInfoEmitter.cpp:2027` | `CalleeSavedRegs` def 名派生的 call-preserved mask。 |
| `get<Name>(HwMode)` | `llvm/utils/TableGen/RegisterInfoEmitter.cpp:228`, `llvm/utils/TableGen/RegisterInfoEmitter.cpp:1436` | `RegisterByHwMode` def 名派生的 getter。 |

<!-- END: GENERATED DSL CONTRACT -->

### 跨后端投影矩阵

同一个字段名通常被多个 `-gen-*` 读取。下面这张表回答“我只改这个字段，还会
影响哪些后端”。`shared` 表示共享派生层（`Common/`、`Basic/`、`Record.h`）
也读它；`⚠` 表示同名多声明。

<!-- BEGIN: GENERATED PROJECTION MATRIX -->

只列“本后端之外还有读者”或“同名多声明”的字段；`shared` 表示共享派生层也读它。

| 字段 | 声明于 | shared | 其它 `-gen-*` | helper | 同名 |
| --- | --- | --- | --- | --- | --- |
| `Objects` | `RegInfoByHwMode`, `SubRegRangeByHwMode`, `RegisterByHwMode` | ✓ | `instr-info` | — | ⚠ |
| `Offset` | `SubRegRange`, `SubRegIndex` | ✓ | — | — | ⚠ |
| `Size` | `SubRegRange`, `SubRegIndex`, `RegisterClass` | ✓ | `asm-matcher`, `callingconv`, `compress-inst-emitter`, `instr-info` | `helper:DecoderEmitter`, `helper:X86RecognizableInstr` | ⚠ |
| `Namespace` | `SubRegIndex`, `RegAltNameIndex`, `Register`, `RegisterClass` | ✓ | `asm-matcher`, `asm-writer`, `emitter`, `fast-isel`, `pseudo-lowering`, `subtarget` | — | ⚠ |
| `FallbackRegAltNameIndex` | `RegAltNameIndex` | — | `asm-writer` | — | — |
| `AltNames` | `Register` | — | `asm-matcher`, `asm-writer` | — | — |
| `AsmName` | `Register` | ✓ | `asm-matcher`, `asm-writer` | — | — |
| `PositionOrder` | `Register`, `RegisterTuples` | — | — | — | ⚠ |
| `RegAltNameIndices` | `Register` | — | `asm-matcher`, `asm-writer` | — | — |
| `SubRegIndices` | `Register`, `RegisterTuples` | ✓ | — | — | ⚠ |
| `SubRegs` | `Register`, `RegisterTuples` | ✓ | — | — | ⚠ |
| `Alignment` | `RegisterClass` | ✓ | — | `helper:X86RecognizableInstr` | — |
| `CoerceFromRegClass` | `RegisterClass` | — | `asm-matcher` | — | — |
| `DiagnosticString` | `RegisterClass` | — | `asm-matcher` | — | — |
| `DiagnosticType` | `RegisterClass` | — | `asm-matcher` | — | — |
| `TSFlags` | `RegisterClass` | ✓ | `instr-info` | — | — |
| `GIZeroRegister` | `RegisterOperand` | — | `global-isel` | — | — |
| `OperandType` | `RegisterOperand` | ✓ | `asm-writer`, `global-isel-combiner` | `helper:X86RecognizableInstr` | — |
| `ParserMatchClass` | `RegisterOperand` | — | `asm-matcher`, `insn-directive-match-table` | — | — |
| `PrintMethod` | `RegisterOperand` | ✓ | `asm-writer` | — | — |
| `RegClass` | `RegisterOperand` | ✓ | `asm-matcher`, `fast-isel`, `global-isel`, `instr-info` | `helper:DAGISelMatcherGen`, `helper:X86RecognizableInstr` | — |

矩阵覆盖 21 个字段（共 59 个）。
<!-- END: GENERATED PROJECTION MATRIX -->

读法：

- `Namespace` 被 6 个后端读，是真正的跨后端字段；
- `AltNames` / `RegAltNameIndices` 只服务 asm-writer / asm-matcher，
  在 register-info 里是 no-op；
- `PositionOrder` 没有任何其它 `-gen-*` 读者，却仍出现在矩阵里，因为
  `Register` / `RegisterTuples` 同名——这正是“只看谁读”不够的证据。

### 派生层改写（Record → 投影之间的模型变换）

Record 里只有声明；从 Record 到投影之间，共享 C++ 会改写它。这张表由
`DERIVATIONS` 元组驱动，每条正则必须在源码里命中，否则 `--check` 失败，
防止表格腐烂。

<!-- BEGIN: GENERATED DERIVATIONS -->

Record 里只有声明；这些是共享 C++ 在投影之前对语义的改写。

| 改写 | Schema 键 | 证据 | 说明 |
| --- | --- | --- | --- |
| `SetTheory expander` | `RegisterClass.MemberList` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:1249` | 把 dag 展开成寄存器集合；决定成员集与默认分配序。 |
| `SetTheory expander` | `CalleeSavedRegs.SaveList` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:1250` | 把 dag 展开成保存顺序，生成 `<name>_SaveList`。 |
| `tuple synthesis` | `RegisterTuples` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:1251` | 在 backend 运行时合成新的 Record（`SynthDefs`），不进入 RecordKeeper。 |
| `tuple rewrite` | `RegisterTuples.SubRegs` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:707` | 把原型的 SubRegs 替换成 zip 出来的 tuple。 |
| `tuple rewrite` | `RegisterTuples.CoveredBySubRegs` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:718` | 合成寄存器强制 `CoveredBySubRegs = true`。 |
| `tuple rewrite` | `RegisterTuples.DwarfNumbers` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:728` | `DwarfNumbers`/`DwarfAlias`/`Aliases` 重置为 Register 类默认值。 |
| `sorting` | `Register.PositionOrder` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:1277`, `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:1282` | 消费端排序；决定寄存器枚举顺序（`LessRecordRegister` 读 `PositionOrder`）。 |
| `inference` | `RegisterClass` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:1369`, `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:2795` …(+1) | 从显式寄存器类推断出缺失的类；只存在于派生模型。 |
| `hw-mode selection` | `HwModeSelect.Objects` | `llvm/utils/TableGen/Common/CodeGenHwModes.cpp:42` | 按 ModeId 把 `Objects[i]` 绑定到 `Modes[i]`。 |
| `parameterization` | `RegInfo` | `llvm/utils/TableGen/Common/InfoByHwMode.cpp:105` | 把 `RegInfo` 三元组转成内部 `RegSizeInfo`（按 HwMode 存）。 |
| `parameterization` | `SubRegRange` | `llvm/utils/TableGen/Common/InfoByHwMode.cpp:187` | 把 `SubRegIndex.Size/Offset` 或 `SubRegRange` 转成内部 `SubRegRange`。 |
<!-- END: GENERATED DERIVATIONS -->

这 11 条改写解释了为什么“直接读 Record”不足以预测产物：`MemberList` 要先过
SetTheory，`RegisterTuples` 会合成全新 Record，`RegisterClass` 可能由推断
产生，`PositionOrder` 在核心排序器里生效。

### RVMin 的真实用法对照

RVMin 是最小实例：32 个寄存器、一个 `GPR`、一个 `CalleeSavedRegs`。
它的 `.td` 只用到 7 个 `let`/`field`。脚本把它们逐个映射到契约：

<!-- BEGIN: GENERATED TD USAGE -->

| 文件:行 | 字段 | 契约状态 |
| --- | --- | --- |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:12` | `FallbackRegAltNameIndex` | asm writer |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:17` | `Namespace` | 消费（derive+project） |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:18` | `HWEncoding` | 消费（project） |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:19` | `AltNames` | asm writer / asm matcher |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:20` | `DwarfNumbers` | 消费（project） |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:25` | `RegAltNameIndices` | asm writer / asm matcher |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:86` | `Namespace` | 消费（derive+project） |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:87` | `HWEncoding` | 消费（project） |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:88` | `AltNames` | asm writer / asm matcher |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:89` | `DwarfNumbers` | 消费（project） |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:92` | `RegAltNameIndices` | asm writer / asm matcher |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:137` | `DecoderMethod` | decoder |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:16` | `Num` | 自定义字段（需自行保证 name-based 读取方存在） |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:85` | `Num` | 自定义字段（需自行保证 name-based 读取方存在） |

共 14 处使用；契约外 0 处。
<!-- END: GENERATED TD USAGE -->

这张表就是本章最直接的结论：

- `Namespace`、`HWEncoding`、`DwarfNumbers` 真正进入 `-gen-register-info` 的产物；
- `AltNames`、`RegAltNameIndices`、`FallbackRegAltNameIndex` 只服务汇编输出，
  对 register-info **完全无效**；
- `Num` 是 RVMin 自己的 `field`，只有读它的 C++（如果有）才有效。

## 深读：契约表里看不出来的规则

### 1. `MemberList` / `SaveList` 不是普通字段

`RegisterClass.MemberList` 的类型是 `dag`，`CalleeSavedRegs.SaveList` 也是。
它们不是靠 `getValueAs*` 读的，而是在 `CodeGenRegBank` 构造时注册给 SetTheory：

```cpp
// llvm/utils/TableGen/Common/CodeGenRegisters.cpp:1249
Sets.addFieldExpander("RegisterClass", "MemberList");
Sets.addFieldExpander("CalleeSavedRegs", "SaveList");
```

所以 `(add X0, (sequence "X%u", 10, 17))` 能展开成寄存器集合，而普通
`list<Register>` 不行。契约表里它们的读取点写的是 `addFieldExpander`，
不是行号意义上的 `getValue`。

### 2. `PositionOrder` 在核心层生效

`Register`/`RegisterTuples` 都声明了 `PositionOrder`，但
`CodeGenRegisters.cpp` 里搜不到这个字符串。它被
`LessRecordRegister::operator()`（`Record.h:2239`）读取：

```cpp
int64_t LHSPositionOrder = Rec1->getValueAsInt("PositionOrder");
int64_t RHSPositionOrder = Rec2->getValueAsInt("PositionOrder");
if (LHSPositionOrder != RHSPositionOrder)
  return LHSPositionOrder < RHSPositionOrder;
```

这个比较器决定寄存器的枚举顺序，也就决定 `X0 = 1, X1 = 2, …`。
所以它是 `-gen-register-info` 的**间接**输入；只看 emitter 会误判为死字段。

### 3. `RegisterCategory` 只有三个名字有效

emitter 硬编码比较三个 def 名：

```cpp
if (Category.getName() == "GeneralPurposeRegisters")   // :2042, :2056
if (Category.getName() == "FixedRegisters")            // :2070
if (Category.getName() == "ArgumentRegisters")         // :2084
```

分别生成 `isGeneralPurposeRegister()` / `isGeneralPurposeRegisterClass()`、
`isFixedRegister()`、`isArgumentRegister()`。其它名字不会报错，也不会有任何
输出——这就是“名字即 API”的典型。

### 4. `AltOrderSelect` 是代码注入点

`RegisterClass.AltOrderSelect` 的类型是 `code`，emitter 把它的原文直接拼进
生成的函数体：

```cpp
OS << "static inline unsigned " << RC.getName()
   << "AltOrderSelect(const MachineFunction &MF, bool Rev) {"
   << RC.AltOrderSelect << "}\n\n"
```

也就是说，`.td` 里的这段 `code` 会成为目标 C++ 的一部分；它不是数据，
不能当普通字符串看。并且只有非空时才生成 `getRawAllocationOrder()`。

### 5. `RegAltNameIndex` 家族对本后端基本无效

- `RegAltNameIndex` 只贡献一个枚举名（且只有 `size() > 1` 时才发射）；
- `Register.AltNames`、`Register.RegAltNameIndices`、
  `RegisterClass.altNameIndex`、`RegAltNameIndex.FallbackRegAltNameIndex`
  在本后端**都没有读取点**——它们是 `-gen-asm-writer` / `-gen-asm-matcher`
  的输入。

RVMin 的 `let FallbackRegAltNameIndex = NoRegAltName in def ABIRegAltName`
是从别处复制来的：对汇编输出有意义，对 `-gen-register-info` 是 no-op。
这类“看起来很重要、实际不生效”的写法，正是契约表要消灭的。

### 6. `DwarfNumbers` 是“flavour 列表”，不是单个编号

每个寄存器给一个列表，第 `i` 个元素对应第 `i` 个 DWARF flavour；
`-1` 表示该 flavour 下无映射。emitter 取全局最大长度，短的补 `-1`，
再按 flavour 生成 `DwarfFlavourN` / `EHFlavourN` 两套表。
`DwarfAlias`（由 `DwarfRegAlias` 提供）会复制被别名寄存器的整组编号。

### 7. `RegisterTuples` 会重写字段

`TupleExpander` 合成的寄存器不是简单复制原型：

| 字段 | tuple 展开时的处理 |
| --- | --- |
| `SubRegs` | 替换成 zip 出来的 tuple |
| `SubRegIndices` | 用 `RegisterTuples.SubRegIndices` |
| `AsmName` | 用 `RegAsmNames[n]`；没给就是空串 |
| `CostPerUse` | 取首个元素（Prototype）的列表，不是各维求和 |
| `CoveredBySubRegs` | 强制 `true` |
| `DwarfNumbers` / `DwarfAlias` / `Aliases` | 重置为 `Register` 类的默认值 |

名字由各维寄存器名用 `_` 拼接；重名直接 fatal。这些规则都不在 `.td` 里。

### 8. 一个字段名，多个类共享：为什么主键是 `(declaring class, field)`

`Size` 同时出现在 `RegisterClass`（spill size）、`SubRegIndex`（子寄存器位数）
和 `SubRegRange`（范围位数）。提取器内部以 `(declaring class, field)` 为主键，
所以 `--json` 能看到三条独立声明；文档里的表把同名声明合并成一行并标 `⚠`，
因为它们的读取点仍按名字共享。

真实的绑定发生在 C++ 的调用点：

- `CodeGenRegisters.cpp:800` 读的是 `RegisterClass.Size`；
- `InfoByHwMode.cpp:188` 的 `SubRegRange(R)` 读的是 `SubRegIndex.Size`
  （因为 `SubRegIndex` 也有这个字段名）。

结论要分两半说：**读取按名字，归属按声明类**。只保留一半，都会得到错误的
契约——只按名字会把无关 schema 混进来，只按类会漏掉跨类共享的读取点。

### 9. 注释会骗人：`CopyCost` 的负值

`Target.td` 和构造函数的注释都说“负值饱和到 255”：

```cpp
// Saturate negative costs to the maximum
if (CopyCostParsed < 0)
  CopyCost = std::numeric_limits<uint8_t>::max();
else if (!isUInt<8>(CopyCostParsed))
  PrintFatalError(R->getLoc(), "'CopyCost' must be an 8-bit value");

CopyCost = CopyCostParsed;   // ← 覆盖了上面的饱和
```

`CopyCost` 的成员类型是 `uint8_t`，最后一行把 `int` 直接赋回去：
`-1` 恰好还是 255，但 `-2` 变成 254，饱和语义并不成立。
这正是契约必须从**代码**提取的原因：注释描述意图，代码才是行为。
（如果这是 upstream 的 bug，修它不在本章范围；本章只负责把它公开。）

## 构建与验证

整套实验只依赖 Python 3 和本章的 `.td`，不需要重新构建 LLVM；只有第 4 步的
`llvm-tblgen` 内省需要构建目录里已有的二进制。

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop
T="$W/llvm/utils/TableGen/dsl-surface.py"
DOC="$W/llvm/lib/Target/RVMin/docs/29-register-info-dsl.md"
TD=("$W/llvm/lib/Target/RVMin/RVMinRegisterInfo.td"
    "$W/llvm/lib/Target/RVMin/RVMinCallingConv.td")

# 1. 契约 + 文档 + RVMin 用法，一次校验
python3 "$T" --check "$DOC" --usage "${TD[@]}"

# 2. 看统计和缺口
python3 "$T" --json | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["stats"]); print(d["problems"])'

# 3. 只看跨后端投影矩阵
python3 "$T" --matrix

# 4. emitter 行号漂移后重新生成（review diff 应只有行号变化）
python3 "$T" --update "$DOC" --usage "${TD[@]}"

# 5. 让 tblgen 自己打印内部模型（验证契约与实现一致）
D=$(mktemp -d)
"$B/bin/llvm-tblgen" -gen-register-info -register-info-debug \
  -I"$W/llvm/lib/Target/RVMin" -I"$B/include" -I"$W/llvm/include" \
  -I"$W/llvm/lib/Target" "$W/llvm/lib/Target/RVMin/RVMin.td" \
  -o "$D/out.inc" 2> "$D/debug.txt"
head -30 "$D/debug.txt"

# 6. 全量门禁（本章起为 7/7）
"$W/llvm/lib/Target/RVMin/scripts/rvmin-verify-all.sh" "$B/bin"
```

## 预期输出

```text
$ python3 llvm/utils/TableGen/dsl-surface.py --check ... --usage ...
dsl-surface: OK (register-info: 44/59 fields consumed [39 derive, 4 project, 1 both], 10 magic names, 11 derivations)

$ python3 llvm/utils/TableGen/dsl-surface.py
register-info: {'roots': 8, 'fields': 59, 'consumed': 44, 'unused': 15,
 'stages': {'schema': 15, 'derive': 39, 'project': 4, 'derive+project': 1}}

$ ... -register-info-debug ...
RegisterClass GPR:
        SpillSize: 32
        SpillAlignment: 32
        NumRegs: 32
        LaneMask: 0000000000000001
        HasDisjunctSubRegs: 0
        CoveredBySubRegs: 0
        Allocatable: 1
        AllocationPriority: 0
        Weight: 1
        BaseClassOrder: None
        Regs: X0 X1 X2 ... X31

$ scripts/rvmin-verify-all.sh "$B/bin"
...
PASS 7/7: TableGen DSL contract (register-info)
ALL GATES PASS (run rvmin-run.sh separately for the QEMU check)
```

## 常见坑

1. **`--check` 失败先看是不是行号漂移。** 多数情况下 `--update` 之后
   diff 只有 `:行号` 变化；如果出现字段增删，才是真正的语义变更，要 review。
2. **不要把 `CodeGenTarget.cpp` 加进 roots 扫描。** 它包含
   `getAllDerivedDefinitions("Target")` / `("AsmParser")` 等，会把 8 个根
   膨胀成 14 个，并带进 `Predicates`、`isPseudo` 这类无关字段。
3. **`getValueAsOptionalDef` 与 `getValue` 语义不同。** 前者字段不存在时返回
   `nullptr`（`RegInfos`、`SubRegRanges` 就是可选的）；后者拿 `RecordVal*`，
   缺失时后续解引用可能直接 crash（`DwarfAlias` 只在继承
   `DwarfRegAlias` 时存在）。
4. **`let` 拼错字段名会被 tblgen 拒绝，但“字段存在、只是本后端不读”不会。**
   后者才是静默失效的来源；用 `--usage` 一眼就能看出来。
5. **`.td` 解析器只覆盖 `Target.td` 的写法。** 没有 `multiclass`/`foreach`/
   复杂 `let` 表达式；新增写法时宁可让脚本报错，也不要静默跳过。
6. **同名歧义是 schema 事实，不是脚本缺陷。** `Size`、`Namespace`、`Objects`
   等 7 个字段确实被多个类独立声明；`--json` 保留
   `(declaring class, field)` 主键，文档表合并并标 `⚠`。如果你试着“去重”
   成一行，就会丢掉 `RegisterClass.Size` 与 `SubRegIndex.Size` 的语义差异。
7. **`--check` 报 `derivation ... no longer matches` 时不要删表项。**
   这说明共享 C++ 的写法变了；先确认改写是否仍然存在，再更新 `DERIVATIONS`
   的正则或说明。删掉表项等于把派生层重新变成不可见 hardcode。

## 练习

1. 给 `BACKENDS` 增加 `instr-info`：先用
   `grep getAllDerivedDefinitions llvm/utils/TableGen/InstrInfoEmitter.cpp`
   找根，再猜哪些字段会出现在“未消费”表里，最后跑脚本对答案。
2. 给 `RegisterClass.Weight` 写一个越界/负数用例，观察 `getWeight()` 与
   emitter 的压力表分别怎么变。
3. 扩展现有脚本，检测“被 `getValue*` 读取但 `.td` 里根本没有声明”的字段。
   `DwarfAlias` 是一个已知的“条件存在”案例，思考怎么表达才不误报。
4. 把生成块提升到 `llvm/docs/TableGen/BackEnds.md`：需要补哪些
   CMake/文档目录的钩子？为什么本章先放在 RVMin 教程里？
5. 用 `-print-records` 或 `-dump-json` 导出 RVMin 的记录，和契约表做一次
   “声明 vs 实际值”的对照，看看还能自动化哪一步。

## 产物清单

| 文件 | 变更 |
| --- | --- |
| `llvm/utils/TableGen/dsl-surface.py` | 新增：契约提取、`--json`/`--matrix`/`--usage`/`--update`/`--check` |
| `llvm/lib/Target/RVMin/docs/29-register-info-dsl.md` | 新增：本章（四个生成块：契约 / 投影矩阵 / 派生改写 / RVMin 用法） |
| `llvm/lib/Target/RVMin/docs/README.md` | 新增“实验课”索引行 |
| `llvm/lib/Target/RVMin/scripts/rvmin-verify-all.sh` | 新增第 7 项门禁 |
