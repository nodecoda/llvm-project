# 生成器走读：从 `X86LegalizerInfo.td` 到 `X86LegalizerTable.inc`

版本：`sync/table-classify` @ `bcd51617a6e3`（2026-11-01）。范围：X86 线，
`llvm/utils/TableGen/LegalizerInfoEmitter.cpp`（3708 行）。

## 0. 入口：一个前端，三份产物

同一个 `LegalizerInfoEmitter` 注册了三个 backend：

| 选项 | 入口 | 产物 | 消费者 |
|---|---|---|---|
| `-gen-legalizer` | `run` (854) | 规则链 C++ 语句序列 | `X86LegalizerInfo.cpp` 构造函数里 `#include` |
| `-gen-legalizer-core` | `runCore` (3491) | Core IR JSON（schema `legalizer-spec-core` 0.1） | 审计 / 差分 |
| `-gen-legalizer-table` | `runTable` (2876) | 键划分 + 描述符 `.inc` | `X86LegalizerTable.cpp` 匿名 namespace 里 `#include` |

`llvm/lib/Target/X86/CMakeLists.txt:24-32` 是两次独立调用、同一份输入。
输入必须是 `GISel/X86LegalizerInfo.td`（它 `include "llvm/Target/LegalizerInfo.td"`）；
传 `X86.td` 会在 TableGen 第一步死在 `LegalizeTypeLattice` 未定义。

**`runTable` 不读 JSON**。它和 `runCore` 共用同一份内存里的展开结果
（`expandRule()` 产出的 `CoreRule`），JSON 只是把那份结果再序列化一次。
所以"表与规则一致"不是靠文件比对，而是靠"同一份输入、同一段展开代码"。

## 1. 阶段 A：读记录 + 共享校验（所有产物之前）

`validatedRuleSets()` (2617) 对每个 `LegalizeRuleSet` 检查：

- opcodes 非空、rules 非空、每个 opcode 以 `G_` 开头、opcode 全局唯一；
- 每条 rule 过 `verifyRule()`。

`verifyRule()` (478) 是手写 C++ 做不到的那几类检查：

| 检查 | 目的 |
|---|---|
| builder 的 `Action` 必须是 `LegalizeAction` | 动作由构造器决定，规则不能另说一套 |
| rule 的 guard 必须是 builder 声明的重载（`GuardBool` / `GuardQuery`） | 防止把常量 bool 传给要 callable 的 builder |
| 参数形状 vs builder 的 `Formals` 重载 | 阻止 `.clampScalar({s32})`、`.lower({s32})` 这类能生成但编译不过的写法 |
| `Mutation` 方向 vs action 要求的 `Mutation` | 动作和 mutation 分居两处，这是唯一把它们绑起来的地方 |
| opaque body 不能同时带 `Pred` / `Args` | 否则会被静默丢弃 |

`verifyLattices()` (640)：类型格必须二阶递增，`clampScalar`/`minScalar` 的
bound 必须落在某个 lattice 上。

"谓词可表达"分两层：

1. **语法可表达**：`predicateJson()` (1941) 对不认识的 predicate record
   `PrintFatalError`；`expandRule()` (2098) 对非字面量类型给出 `Why`。
2. **键可推断**：`collectPredLeaves()` (2739) 只认识一张 leaf 白名单，
   不认识的 leaf 直接 fatal（"is not one this table knows how to key"）；
   raw C++ 谓词（`LegalizeOpaqueBool`）不 fatal，而是标记为不表化。

## 2. 阶段 B：Core IR

`expandRule()` 把一条 `.td` 规则展开成 0..N 条 `CoreRule`：

```cpp
struct CoreRule { std::string Predicate; std::string Mutation; unsigned TypeIdx; ... };
```

- `LegalFor<[s8, s16, s32]>` → 三条 CoreRule（`rule_index` 都是 0，因为它们来自同一条 record）；
- `ClampMaxNumElements<0, X86_s8, "HasBWI ? 64 : ...">` → 按分支 residualize，
  每条带一个 `hasFeature` guard；
- opaque / 非字面量 / 类型列表选择 → 产生不了 CoreRule，记 `Why`（`opaque_rule`、
  `nonliteral_type`、`type_list_choice`、`inline_type_expr`、`raw_argument`）。

X86 的数字（`llvm-tblgen -gen-legalizer-core`，本树实测）：

```
schema legalizer-spec-core 0.1
rulesets 63   rules 570   escapes 0
domain: types 24   mems 23   imms 0
max rules/ruleset 42
```

`X86_ADD` 的 17 条 record 展开成 28 条 CoreRule（`LegalFor` 按类型展开成 16 条，
`ClampMinNumElements` 4 条，`ClampMaxNumElements` 4 条——lane 数是运行期求值的
C++ 表达式，不 residualize，`WidenScalarToNextPow2` 2 条，`ClampScalar` 2 条，
`Scalarize` 1 条）。最后几条是：

```
24 WidenScalar  set_elem_bits nextPow2OrMin32  sizeNotPow2(0)
25 WidenScalar  set_elem_type s8               scalarNarrowerThan(0,"8")
26 NarrowScalar set_elem_type s64              scalarWiderThan(0,"64")
27 FewerElements set_lanes 1                   isVector(0)
```

## 3. 阶段 C：表（`runTable`）

### C1 键依赖

对每条 CoreRule 的 predicate JSON 扫叶子：

- 读槽 leaf（`typeIs`/`typeIn`/`isVector`/`sizeIs`/`scalarNarrowerThan`/…）→ `Slots`；
  同时 `typeIs`/`typeIn` 的字面量进 `SlotTypes`（既做自检域，也进类集）；
- `memSize` → `MemSizes`；`typeDiffersFromMemDesc` → 需要按类型切 mem 轴（`MemNeedsClass`）；
- `hasFeature`/`true` 不进键（每个 subtarget 一个 `X86LegalizerInfo` 实例，feature 是常量）；
- `opaque` → `HasOpaquePredicate`。

写槽（`ActionSlots`）和 mutation 读槽（`MutationReadSlots`，`sameAsSlotN`）
**不进键**，只用来判 `OneQuestion`。

### C2 拒绝清单（不是 gap）

三类不表化的 rule set，各自一个 vector，最后写成 `.inc` 头部的注释：

| 清单 | 触发条件 |
|---|---|
| `EscapeRuleSets` | 规则展开失败（opaque / 非字面量 / 类型列表选择） |
| `OpaquePredicateRuleSets` | 谓词是 raw C++，键不知道它读哪些槽 |
| `UnkeyableRuleSets` | mem 轴要按类型切，但没有任何规则写出 memory type |

X86 当前三张清单都是 0 条（`grep -c "Not tabulated"` = 0）。
没有表的 opcode 在运行时不报错：`x86TableForOpcode` 返回 -1 → `Untabulated++` → 回退链。

### C3 类集

`Types` = `domainTypes()`（.td 声明的类型 + `p0:64`）∪ 所有规则里出现的字面量，
排序去重后 `parseTableClass()`（2702）转成 `(kind, elem, lanes)`：

```
Kind_Pointer=0  Kind_Scalar=1  Kind_Vector=2  Kind_PointerVector=3
```

结果是**全局 24 个类**，所有表共享：

```
0 p0:64   1 s1   2 s8   3 s16  4 s32  5 s64  6 s80  7 s128
8..21 向量  22 v2p0:64  23 v4p0:64
```

生成的 `X86ClassifyType` 是以 `X86ClassKey(kind, elem, lanes)` 为 case 的 switch；
`X86ClassKey` 的位布局（kind<<48 | elem<<16 | lanes）带两个 `static_assert`，
防止 element 位域溢进 kind（i65537 那次 bug）。

### C4 键划分与 local 映射

- `T.Slots` = 排序后的**读槽**集合；>2 个类型槽直接 fatal；有 mem 轴则追加 `-1`；
- `Dims[K]` = 类型槽 `Classes.size()`=24 / mem 轴 `Mem.size()` / 未用 1；
- `Local` 数组：**目前恒等**（`for C in 0..Count: emit C`）。

所以"局部类映射"今天不压缩任何东西：cell 下标就是全局类 id。
真实 cell 数（每个 subtarget 实例、按需分配）：

| 表 | Slots | Dims | cell 数 |
|---|---|---|---|
| X86_ADD | {0} | 24×1×1 | 24（自检域 19） |
| X86_LOAD | {0,1,-1} | 24×24×9 | 5184（域 3×1×9） |
| X86_STORE | {0,1,-1} | 24×24×24 | 13824（域 3×1×24） |

恒等不是浪费：把 Dims 压到域内，就必须让"域外类"从"填表问链"变成
"直接 Outside"，那是行为改变，不是纯优化。

### C4b 为什么是这个键（论证）

键 = `opcode → table` + 至多两个类型槽的**全局类 id** + 可选的 mem 轴
（访问位宽，或类 id）。

设规则集的规则为 `r1..rn`，`findRule(Q)` 是第一个满足 `p_i(Q)` 的 i。表要正确，
只需 `κ(Q) = κ(Q') ⇒ p_i(Q) = p_i(Q')` 对所有可表化规则成立——也就是说，键必须是
每个谓词真值集的**细化**（refinement）。反过来越粗越好：越粗，格子越少。

**类型的类 `(kind, elem, lanes)` 足够**，因为每个叶子只读这些坐标：

| leaf | 读的坐标 |
|---|---|
| `typeIs` / `typeIn` | 整个字面量 |
| `isVector` / `elementTypeIs` | 是否向量、元素 kind+elem |
| `sizeIs` / `sizeNotPow2` | 总位数（= elem × lanes；`sizeNotPow2` 还要求 scalar） |
| `scalarNarrowerThan` / `scalarWiderThan` / `scalarOrEltWiderThan` / `scalarOrEltSizeNotPow2` | elem 位数 |
| `numElementsNotPow2` / `fewerElementsThan` / `moreElementsThan` | lanes |
| `scalarSizeLargerThanSlot` / `scalarSizeSmallerThanSlot` | 两个槽的 elem 位数 |

没有一条读 LLT 的 integer/float 区分：规则都写成 `LLT::scalar(N)`，而 ANY_SCALAR
在 `LLT::operator==` 里是通配。所以类里不放这个区分——这正是**答案缓存**必须
单独门控 `!LLT::getUseExtended()` 的原因。如果规则真写了 kind 敏感的类型（`i64`），
`parseTableClass` 不认识这个前缀，生成器 fatal，不会静默合并。

**mem 轴为什么两态**：`memSize` 和 `isCompatible` 只比访问位宽，位宽就是精确的商；
`typeDiffersFromMemDesc` 比整个 `MemoryTy`，两个同宽不同类型会分开（v2s32 vs s64），
所以轴要按类 id 切（`MemIsClass`）。规则读整类型却没写出任何 memory type 时，
轴没有可枚举的有限集合 → `UnkeyableRuleSets`，拒绝建表。

**不进键的坐标，以及为什么安全**：

| 坐标 | 为什么可以不在键里 |
|---|---|
| subtarget feature | 每个 `X86LegalizerInfo` 实例的常量（`X86Subtarget.cpp:340`）；进键只会让表变成 per-subtarget |
| immediate | 没有 core leaf 读它；读它只能写成 opaque C++，而含 opaque 谓词的规则集不表化 |
| 未 key 的类型槽 | 没有谓词读它，规则号与之无关；重放时 mutation 用真实查询重算 |
| 写槽 / mutation 读槽 | 规则号不依赖它们；只有**答案缓存**需要，所以它们进的是 OneQuestion 条件，不是键 |
| alignment | 规则只问 "at least"，`memSize` 要求 align==1，代表元取最弱对齐 |
| 地址空间 | 类表只写 p0；AS≠0 直接 Outside（回退链），不是与 p0 合并 |
| LLT kind（int/float） | 谓词通配（见上）；答案缓存需要，所以单独门控 |

**粗一点 / 细一点的代价**：

| 候选键 | 后果 |
|---|---|
| 只按总 size | v2s32 与 s64 同格，规则不同（vector 分支 vs scalar 分支）→ 错答 |
| 按完整 LLT | 类爆炸且大量不可达；规则不读 kind/AS |
| 每 opcode 一格 | 所有类型同格 → 错答 |
| 把 immediate / 写槽加进键 | 格数 ×N，规则号不变；答案缓存才需要（条件式） |

结论：键是"每个可表化谓词都恒定的最粗划分"。它由 `collectPredLeaves` 从 Core IR
**推导**，不是手写清单：规则新增一个读槽，键自动变宽；遇到不认识的 leaf 或 opaque
谓词，生成器要么 fatal 要么拒绝建表，不会猜。

键还是**偏函数**：域外的类型 / 内存类型不给错答案，而是 `Outside` / `Declined` /
`PointerVector` / `Split` → 回退链。这也是为什么 `Dims` 用全局类数（24）而不是
每表域——域外类照样有格子，由 `fill` 问链。

可测性来自域的有限性：2866 个域点可以枚举（`selfCheck`），真实查询由 check 模式比对
（语料 92352 次）。键的每一个"细化"主张都有一个能红的检查。

### C5 自检域

per-slot 域，不是全类笛卡尔积（`SlotDomains`）：

- 规则在该槽点名的类；
- 若该槽有指针类 → 只留指针类（指针槽问标量是链不接受的问题）；
- 若该槽有标量类 → 补全所有标量类（`scalarNarrowerThan` 这类不点名的宽度边界也要在域里）。

生成 `X86DomainT*` + `LegalizerTableDomain`，喂给 `-x86-gisel-table-check`。

### C6 描述符与 OneQuestion

`LegalizerTableDesc`：`NumSlots / Slots / Dims / Local / Mem / MemIsClass /
FirstRule, NumRules / OneQuestion`。

`IsOneQuestion`（3393）四个条件：无 mem 轴、`Slots[K] == K`、
写槽 ⊆ 键槽、mutation 读槽 ⊆ 键槽。X86：**56/63**。

### C7 opcode 映射

`x86TableForOpcode` 是按 `TargetOpcode::G_*` 的 switch（115 个 opcode，生成期解析，
运行时不带字符串表）；另有 `X86TableOpcodes[]` 给自检用。

## 4. 阶段 D：运行时怎么用（一句话版）

`classifyType(LLT) -> 类 id -> Local/Dims -> cell`；
cell 里是 `RulesOfCell`（链的规则号 `findRule`）或（OneQuestion）缓存的答案。
`Outside / Declined / Split / Untabulated` 都回退链，各自的计数在 stats 里。

## 5. 正确性：4.1 的每条要求对应哪个不变量

4.1 列的是"必须成立的性质"，不是"代码该长什么样"。逐条给不变量、实现怎么保证、
什么会破坏它、以及现在由谁测。

| 4.1 要求 | 不变量（正确性命题） | 实现里的保证 | 什么会破坏它 | 测试 |
|---|---|---|---|---|
| 动作/mutation 一致 | 规则写的方向在 action 允许的方向内（`any` 放行复合动作） | `verifyRule` 比对 `Action::Mutation` 与 `Rule::Mutation` | 手写 C++ 链没有这个连接点 | `legalizer-info-bad-mutation.td` |
| 谓词可表达 | 只有白名单叶子能进键；不认识的不能猜 | `predicateJson` / `collectPredLeaves` 都 fatal；raw C++ 不猜，改为不表化 | 新增 leaf 忘了同步白名单（是 fatal，安全） | `legalizer-info-core-unknown-predicate.td` |
| 键划分 / cell 里是 plan id | cell 的内容 = 该格子 first-match 的规则号，且在格子内恒定 | `fill` 用代表元问**链** `findRule`，`RulesOfCell` 存号；查询时用真实查询 `applyRule` 重放，`ruleMatches` 守卫；check 模式再比 `findRule(Q) == RuleIdx` | 键漏一个谓词读槽 → 格子合并两个问题（`splits`/`ruleMatches` 只能抓一部分） | `table-check.ll`（check 模式）+ 语料 92352 次 checked |
| cell 是 subtarget 的答案 | 同一个类在不同 subtarget 上赢的规则可以不同 | `.inc` 只存键和描述符；`X86Subtarget` 每个实例一个 `X86LegalizerInfo`，cell 懒填充 | 把 cell 烘进生成物（跨 subtarget 复用同一个答案） | 结构性：feature 不进键，靠 per-subtarget 实例（`X86Subtarget.cpp:340`） |
| 局部类映射 | `Local` 是全局类→局部下标的全函数，且像上单射；`localToGlobal` 是它的逆 | 生成恒等映射，`Dims = 类数`，恒等时两条都平凡成立 | 压缩时两个全局类映射到同一个局部下标 → `fill` 用错代表元；`localToGlobal` **取第一个匹配、不报错** | 无显式检查（恒等时平凡；压缩前必须加检查） |
| 无法求值 → 拒绝建表 | 没有键支撑的规则集不得回答 | 三张清单 + 回退链 + `Untabulated` 计数 | 把 escape 当可表化 | `legalizer-info-table-escape.td`（三张清单各自的负例，`Test_CORE` 是对照）、`legalizer-info-param-bounds.td` |
| 产物是静态数据 | 生成物与规则同源、不签入 | 同一 `.td` 两次 tablegen | — | 重建逐字节一致 |

所以上次那三处，正确性上都成立，是我把"描述粒度不同"说成了"偏差"：

- **不在 `.inc` 里放 cell 是必须的**：cell 是 subtarget 的答案（feature 不进键），
  烘进生成物就会跨 subtarget 复用同一个答案。"plan id" 在运行时就是规则号。
- **恒等 local 映射是保守正确的**：`Dims = 类数` 保证任何类 id 都落在界内，
  域外类照样进格子、由 `fill` 问链。压缩不是纯优化，它引入两个新不变量
  （单射、域外类变 Outside）。
- **拒绝建表是正确行为**，而且有合成 `.td` 的负例测试；运行时回退链并计数。

真正的风险只有一处：`localToGlobal` 的单射前提没有被显式检查。今天恒等所以没事，
等谁把 `Local` 压成稠密映射时才发作，而且是静默的——它取第一个匹配，不报错。

## 6. 卫生问题（不是正确性问题）

1. `FirstRule/NumRules` 是死字段：570 条规则的连续区间，没有任何代码读。
2. `Local[]` 恒等、`Dims` 恒 24：压缩前要先写下单射检查（见第 5 节）。
3. 生成物头部引用了树外的 "07 section 2.2"。
4. 类型槽硬上限 2（`Slots.resize(2)` + fatal）；`Dims` 是 `uint8_t`，类数 >255
   会在生成代码里变成窄化错误（fail-closed）。
