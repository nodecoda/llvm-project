# 21. 第六轮语料：ub / ptr / shard / arm_sve

> 配套文件：[`corpus/ub.dialect`](corpus/ub.dialect)（33 行、2 op）、
> [`corpus/ptr.dialect`](corpus/ptr.dialect)（249 行、13 op）、
> [`corpus/shard.dialect`](corpus/shard.dialect)（346 行、22 op）、
> [`corpus/arm_sve.dialect`](corpus/arm_sve.dialect)（369 行、44 op）；
> [`dialect-grammar.y`](dialect-grammar.y)（**58 条规则 / 138 条产生式**，0 冲突）；
> 25 个语料文件全部 `parse OK`。
>
> 与 19 章的关系：规则第二次被压测。81 个新 op 换来 **0 条 op 形状产生式**、
> 一个**核心词**（`same_type`）；同时 `qualified(type(x))` 凑齐第三个方言见证，
> 按 R4 判据升级为**文本产生式**并在本轮落地。

## 0. 这一轮选这四个的理由

| 方言 | 检验点 |
| --- | --- |
| `ub` | 极小方言：2 个 op、1 个**无参数属性**；毒值是"纯的但不能用" |
| `ptr` | 类型重心：不带元素类型的不透明指针；4 个枚举（含位集枚举） |
| `shard` | 符号不是类型：`grid` 是符号 op；12 个集合通信共用一个 schema |
| `arm_sve` | 家族压缩：44 个 op 压成 13 个 schema；同一概念的两种文本通道 |

四个方言分别压四条边：**属性无负载**、**类型无元素类型**、**符号无类型**、
**op 数量无文法增长**。

## 1. 语料

| 文件 | 行数 | dialect | constraint | enum | attr_def | type_def | schema | op |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `ub.dialect` | 33 | 1 | | | 1 | | | 2 |
| `ptr.dialect` | 249 | 1 | 6 | 4 | | 2 | 2 | 13 |
| `shard.dialect` | 346 | 1 | 8 | 1 | | 1 | 1 | 22 |
| `arm_sve.dialect` | 369 | 1 | 4 | | | | 13 | 44 |
| 其余 21 个方言 | 4736 | 21 | 48 | 29 | 13 | 30 | 60 | 438 |
| **合计** | **5739** | **25** | **66** | **34** | **14** | **33** | **76** | **519** |

## 2. 文法增量：0 条 op 形状产生式，1 条文本产生式，核心词表 +1

81 个 op、四个陌生领域，没有逼出任何 op 形状产生式。但词表与文本层各动了一处：

### 2.1 `same_type` 升级进核心（39 → 40）

pdl_interp 早就有 `same_type_operands`（句柄类型相等），当时按本地事实处理。
这一轮 `shard` 独立需要同一个概念：`AllTypesMatch<["result","src"]>` 与
`TypesMatchWith<"result 与 destination 同型">` 要求**两个部件的完整类型相等**，
而核心表只有 `same_shape`、`same_element_type` 两个较弱的投影。

按 19 章 R5 第 1 条（同一概念一个词）与第 4 条（≥2 方言见证可进核心）：

| 动作 | 结果 |
| --- | --- |
| `same_type_operands` → `same_type` | 一个词，3 处使用（pdl_interp 1、shard 2） |
| 升级进核心表 | 核心 39 → **40**，本地 19 → **18** |
| 单次使用 | 7 → **6**（33%） |

这是五轮以来第一次"扩语料反而让本地词表变小"。

### 2.2 `qualified(type(x))`：第三个见证，本轮落地为产生式

同一个缺口现在有三个独立见证：

| 方言 | 位置 | 上游写法 |
| --- | --- | --- |
| sparse_tensor | `SparseTensorTypes.td:67` 等 | 类型必须带方言全名 |
| emitc | `EmitC.td` 的 call 类 | 打印时强制限定 |
| ptr | `PtrOps.td` 的 load/store/masked/gather | `qualified(type($ptr))` |

按 18 章 R4 的判据——**有固定语法位置、可嵌套**——它不是事实，是产生式。
本轮落地，一共三件事：

| 改动 | 内容 |
| --- | --- |
| 文法 | `A_QUALIFIED "(" A_TYPE "(" IDENT ")" ")"` 一个元素、一条产生式 |
| 词法器 | `qualified` 关键字（assembly 模式） |
| 语料 | 11 处使用点：ptr 4、sparse_tensor 5 个 op / 6 处、emitc 2 |

改完仍是 0 冲突、25/25 `parse OK`；产生式 137 → 138。这是自第五轮
`Assembly Generic;` 以来新增的第一条产生式，也是第一条**由跨方言见证计数
驱动**、而不是由单个方言的新形状驱动的产生式。

## 3. arm_sve：44 个 op 压成 13 个 schema

44 个 op = **21 个用户可见 op + 23 个 `intr.*`**。家族压缩靠 `from`/`extends`：

| schema | op 数 | 共享的文本形状 |
| --- | --- | --- |
| `SveAccumulate` / `SveIndexedAccumulate` | 6 + 1 | `acc, src1, src2 [, lane] attr-dict : type(src1) to type(dst)` |
| `MaskedArithmetic` | 9 | `mask, src1, src2 attr-dict : type(mask), type(res)` |
| `IntrMaskedBinary` | 9 | generic |
| `IntrBinary` / `IntrIndexedBinary` | 5 + 1 | generic |
| `SvboolConvert` / `SvboolConvertTo` | 1 + 1 | `source attr-dict : type(result)` |
| `Zip2` / `Zip4`、`IntrZip2` / `IntrZip4` | 2 + 2 + 2 + 2 | 交织 |
| `IntrUnary` | 2 | generic |
| 独立 | 5 | psel / dupq_lane / whilelt 及其 intr |

三个领域事实：

1. **scalable 向量是内建类型，不是方言类型。** `SVBool`/`SVEPredicate`/
   `SVEVector` 在 TD 里是内建 `vector` 的约束别名，所以语料里只有
   `constraint svbool = vector`，没有 `type_def`。**约束名不等于类型名**——
   这与 18 章"声明名 = 助记符"是同一条纪律的另一面。
2. **Generic 不是"没有格式"，是"intrinsic 的机器形式"。** 21 个用户可见 op
   有块格式，22 个 `intr.*` 没有 `assemblyFormat` 因而走 Generic；唯一的
   例外 `intr.bfmmla` 自己带声明式格式、走块格式。同一概念两种通道，
   Generic 是 lowering 的稳定契约，不是历史遗留。
3. **家族只写差异。** 14 个 op 的 `from` 行不重复部件，只写 `Doc` 与
   `commutative`；这是 15 章 schema 机制最大规模的一次使用。

GAP：`vector<[N]xT>` 的 scalable 维长与元素类型组合写不进 `constraint`
（只能退化成 `= vector`）；svbool 转换"结果尾维被改写成 16"是类型等式，
没有对应事实。

## 4. ptr：类型是主角，op 只是访存

13 个 op、2 个类型、4 个枚举、6 个约束，**0 Custom / 0 Generic**——

- `type_def ptr` 只参数化内存空间；元素类型不跟指针走，只出现在
  load/store 的值类型上，或由 `type_def ptr_metadata` 显式描述。
- `PtrDiffFlags` 是位集枚举（`: bits`），与 arith 的 flags、gpu 的
  reduction 同形；`AtomicBinOp` 的 `and/or/xor` 为避开关键字拼成
  `_and/_or/_xor`，文本也是这三个拼写。
- `AtomicBinOp` 枚举存在，但本树的 Ptr 方言**没有 atomicrmw op**。
  语料按 TD 声明枚举，不发明 op——"枚举被声明"不等于"有使用者"。
- 掩码访存的形状关系（mask 与值/结果同形）用核心 `same_shape` 表达。

GAP：`memory_space` 上游是 `MemorySpaceAttrInterface`、`ptr_metadata` 的参数是
`PtrLikeTypeInterface`；本语言没有"接口约束"，只能写 `attribute`/`any` 并在
Doc 里说明。

## 5. shard：符号不是类型

最强的"不要发明类型"见证：`shard.grid` 是**符号 op**，给设备网格起名并给出
每轴长度；其余 op 用符号引用指向它。TD 里没有 `GridType`，语料也不写
`type_def grid`。唯一的类型是 `!shard.sharding`——无参数、文本就是裸类型名。

- 12 个集合通信 op 共享 `Collective` schema：设备组由 `grid` + `grid_axes`
  选择，各 op 只补轴号、归约方式或根设备属性。
- `AttrSizedOperandSegments` 出现在 `sharding`/`shard_shape` 上，用核心事实
  `operand_segments_by_attribute` 表达——这是该事实第一次跨三个以上方言使用。
- `same_type` 的第二个见证（`shard` 与 `update_halo`）。

GAP：`AnyNon0RankedTensor/MemRef` 要求 rank ≥ 1，约束语法没有下界写法。

## 6. ub：空负载与"纯的毒值"

`ub` 只有 2 个 op、1 个属性，但补上了语义光谱的两端：

```text
attr_def poison { Assembly { "" } }          /* 文本就是 #ub.poison，无负载 */
op poison      { constant_like, pure }       /* 任何类型的结果 */
op unreachable { terminator }                /* 执行即 UB */
```

- **无参数属性需要显式空串。** `Assembly { "" }` 表示"不打印任何负载"；
  空块 `{ }` 不合法，因为块格式至少要有一个元素。这是"显式优于省略"（R1）
  在属性层的回声。
- **`pure` 说的是无副作用，不是可安全执行。** 毒值没有副作用、可以常量折叠，
  但使用它就是把 UB 推迟到使用者——`pure` 与"值是否可用"是两根轴。
- `unreachable` 是终结符但不是"纯"的：它终止路径，TD 也没有给
  `NoMemoryEffect`；语料照录，不替上游补 trait。

GAP：`PoisonAttrInterface` 接受任意实现该接口的毒值属性，语言只能写可见的
默认类型 `poison`；默认空值写作 `= none`。

## 7. 预算与趋势

| 轮次 | 方言 | 累计 op | 累计产生式 | 本轮新增 op | 本轮新增产生式 |
| --- | --- | --- | --- | --- | --- |
| 15 | 6 | 77 | 98 | 77 | 98 |
| 16 | 9 | 119 | 128 | 42 | 30 |
| 17 | 14 | 199 | 130 | 80 | 2 |
| 18 | 19 | 346 | 136 | 147 | 6 |
| 20 | 21 | 438 | 137 | 92 | 1 |
| **21** | **25** | **519** | **138** | **81** | **1**（文本通道） |

预算表（19 章 R6）：

| 指标 | 当前值 | 红线 |
| --- | --- | --- |
| `ops without a text channel` | 0 | 0 |
| `Assembly Custom` 占 op 比 | 68/519 = 13% | 25% |
| 单次使用的本地事实 | 6/18 = 33% | 50% |
| 跨 ≥3 方言的非核心事实 | 0 | 0 |
| 核心词表 | 40 | — |
| 外部署名 | 12 个 / 7 个方言 | 逐条可辨认 |

最后 173 个 op（第五、六轮）只换来 **2 条产生式（都是文本通道，不是 op 形状）、
1 个核心词、0 个本地词**。新增 op 与新增词的比例已经降到 81:1，这是词表收敛的
直接证据；而唯一新增的产生式来自"同一缺口第三次出现"，不是来自新方言的形状。

## 8. 缺口判决

| 缺口 | 见证 | 判决 |
| --- | --- | --- |
| `qualified(type(x))` | sparse_tensor / emitc / ptr | **已升级为产生式**（本轮，11 处使用点） |
| scalable 维 `[N]` | arm_sve | 记录：类型参数里的量词，不是文本结构 |
| svbool 尾维改写 | arm_sve | 记录：类型等式，不写成事实 |
| rank ≥ 1 下界 | shard | 记录：约束语法缺下界 |
| `memory_space` 接口 | ptr | 记录：缺接口约束 |
| `PtrLikeTypeInterface` | ptr | 记录：缺接口约束 |
| `PoisonAttrInterface` | ub | 记录：缺属性接口 |
| `AtomicBinOp` 无 op | ptr | **不是缺口**：TD 事实，语料照录 |

## 9. 机械核对

```bash
bison -Wall -Wcounterexamples -Werror -o /tmp/dg.c dialect-grammar.y
bison -d -o /tmp/dg.c dialect-grammar.y
cc -I/tmp -o /tmp/dg /tmp/dg.c tools/dialect-lex.c
/tmp/dg corpus/*.dialect
python3 tools/dialect-corpus.py
```

| 检查 | 结果 |
| --- | --- |
| bison 冲突 | 0 |
| 语料解析 | 25/25 `parse OK` |
| 声称但缺失的 op（对 TD 名单） | 0（4 个方言逐名单比对） |
| 部件写错子句（operand↔attribute） | 0 |
| `from`/`extends` 悬空 | 0 |
| Assembly 引用未声明部件 | 0 |
| 死声明 | 0 |
| C++ 泄漏 | 0 |
| 无文本通道的 op | 0 |

事实核对抓到并已修：`intr.bfmmla` 被误写成用户可见的 `bfmmla`（硬错误，
TD 里只有 `intr.` 形式）；`IntrZip` 的结果是 `!llvm.struct` 而非 scalable
vector；ptr 的三个 `*_1d` 约束漏了 rank 1；shard 的 `non0_*` 漏了 rank ≥ 1；
ub 的 value 是带默认值的接口属性而非 `optional`。

## 10. 下一轮

1. **实施 `qualified(type(x))`**：一个产生式 + 三个方言的使用点清扫。
2. 候选方言：`spirv`（434 个 def，枚举/属性最重）、`openmp`（子句与 region
   组合最多）、`amdgpu`（46 个 op、地址空间）、`nvvm`。
3. 复审本轮新增的 7 个 GAP：接口约束（3 个）是否值得一类新约束形式，
   还是永久留在 Doc。

## 11. 结论

1. **op 数量与文法大小已经脱钩。** 81 个 op → 0 条 op 形状产生式；最后 173 个 op
   只加 2 条产生式，且都是文本通道。
2. **词表也在收敛。** 本轮唯一的新词 `same_type` 直接进核心；本地词表
   反而从 19 降到 18。
3. **同一个缺口第三次出现，就该改文法。** `qualified` 已有三个方言见证，
   本轮把它变成产生式并扫清 11 处使用点——留成 Doc 就是把已知的债伪装成缺口。
4. **"没有类型"和"没有格式"都是领域事实。** shard 的 grid 是符号、
   arm_sve 的 scalable vector 是内建类型——不发明类型是正确做法。
5. **Generic 是契约，不是历史遗留。** arm_sve 的 23 个 `intr.*` 证明：
   同一概念可以同时有可读形式与机器形式，两者都该显式声明。
