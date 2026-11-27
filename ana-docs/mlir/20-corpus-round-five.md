# 20. 第五轮语料：emitc / sparse_tensor

> 配套文件：[`corpus/emitc.dialect`](corpus/emitc.dialect)（519 行、55 op）、
> [`corpus/sparse_tensor.dialect`](corpus/sparse_tensor.dialect)（543 行、37 op）；
> [`dialect-grammar.y`](dialect-grammar.y)（**58 条规则 / 137 条产生式**，0 冲突）；
> 21 个语料文件全部 `parse OK`。
>
> 与 19 章的关系：19 章立了逃逸点的规矩，这一轮是**规矩第一次被真实语料压测**——
> 92 个新 op 换来 0 条新产生式，但把两条既有语义读窄的地方暴露了出来。

## 0. 这一轮选这两个方言的理由

| 方言 | 检验点 |
| --- | --- |
| `emitc` | **类型是主角**：C 的类型系统搬进 MLIR，55 个 op 里只有 4 个 schema，其余全是类型与属性的形状 |
| `sparse_tensor` | **属性即数据结构**：`encoding` 的文本是一个有序字典，值的每一支还带级别标签 |

两者都是"属性层/类型层很厚、op 层很薄"的方言，正好补上前四轮偏重 op 形状的盲区。

## 1. 语料

| 文件 | 行数 | dialect | constraint | enum | attr_def | type_def | schema | op |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `emitc.dialect` | 519 | 1 | 12 | 1 | 1 | 7 | 4 | 55 |
| `sparse_tensor.dialect` | 543 | 1 | 6 | 5 | 2 | 4 | 2 | 37 |
| 其余 19 个方言 | 3673 | 19 | 30 | 23 | 10 | 19 | 54 | 346 |
| **合计** | **4735** | **21** | **48** | **29** | **13** | **30** | **60** | **438** |

## 2. 文法增量：0 条产生式，2 处语义修正

92 个 op、两个陌生的领域，一条新产生式都没逼出来。这是 15–18 章归纳质量的
又一次验证；但**语义**上发现两个读窄的地方：

### 2.1 `oilist` 不是"至多一支"

13 章 §5.6 早已照 `OpFormatGen.cpp` 写清楚：

```text
oilist_directive   : KW_OILIST oilist_separator_opt '(' oilist_clause_list ')' ;
```

`mlir/docs/DefiningDialects/Operations.md:808-819` 的定义是"顺序无关的可选子句
列表，**每支 0 或 1 次**"。也就是说两支可以同时出现。而 15 章起我们一直按
"至多一支"理解（`gpu.barrier` 的 `memfence`/`scope` 被写成互斥）。

`emitc.call` 是反例：`arg_attrs` 与 `res_attrs` 可以同时存在。语料因此一度
把这条事实挪进注释：

```text
/* 顺序无关；两支可以同时出现（各 0 或 1 次）。 */
oilist("arg_attrs" "=" arg_attrs | "res_attrs" "=" res_attrs)
```

**这不是语法缺口，是语义读窄。** 语法本来就是对的（`|` 分隔的分支列表），
改的是文法注释与文档措辞。

### 2.2 generic 必须显式（19 章 R1 的第一次落地）

`emitc.constant`/`variable`/`member`/`member_of_ptr` 既无 `assemblyFormat`
也无 `hasCustomAssemblyFormat`，正是 `Assembly Generic;` 的四个见证。

## 3. 类型是主角：emitc

`emitc` 的价值在于**类型层的语义约束**，这些约束在 ODS 里只能写成 verify 代码，
而在本语言里是 `type_def` 的参数：

| 类型 | 文本 | 领域约束 |
| --- | --- | --- |
| `opaque` | `!"<" value ">"` 即 `!emitc.opaque<"int">` | 原样 C 类型名；值不以 `*` 结尾 |
| `array` | `!emitc.array<shape "x" element>` | 元素不能是 array |
| `ptr` | `!emitc.ptr<element>` | 不指向 lvalue |
| `lvalue` | `!emitc.lvalue<element>` | 不包裹 array、不嵌套 |

两个同名不同空间的对象：`type_def opaque`（`!emitc.opaque<"...">`）与
`attr_def opaque`（`#emitc.opaque<"...">`）。**类型空间与属性空间分开**
是这一轮唯一一个"同名不冲突"的见证。

`constant` 拒绝字符串属性、`cast` 的裸词 `pure`（`source if pure { "pure" }`）、
`expression` 的 region 即 C 表达式树——这三条都写进了语料。

## 4. 属性即数据结构：sparse_tensor

`encoding` 的文本是一个有序字典：

```text
#sparse_tensor.encoding<{
  map = (d0, d1) -> (d0 : compressed(nonunique), d1 : singleton),
  posWidth = 40, crdWidth = 24, ...
}>
```

用本语言写出来是：

```text
attr_def Encoding {
  Parameters (dim_to_lvl: affine_map, lvl_types: array<LevelFormat>, ...);
  Assembly { "{" "map" "=" dim_to_lvl ":" lvl_types
             "," "posWidth" "=" pos_width ">" }
}
```

**键是字符串字面量、值是部件**——这不是"自由文本"，而是"键值对列表"的
一种具体拼法。语言不需要知道 `map`、`posWidth` 这些键，它只需要表达
"哪些键、按什么顺序、值是谁"。

`type_def level_type` 是第二个形状：枚举头 + 两个独立可选后缀

```text
structured if n { "[" n "," m "]" } if properties { "(" properties ")" }
```

即 `structured[2, 3](nonordered)`。与 `emitc.array` 的"固定后缀"不同，
这里是**两个可分别出现的后缀**，`if` 正好表达。

## 5. 缺口判决

| 缺口 | 见证 | 判决 |
| --- | --- | --- |
| 字典键值列表 | `SparseTensorAttrDefs.td:121` | **已够用**：键写成字符串、值写成部件 |
| 标签化结果组 `(d0 : compressed, d1 : singleton)` | 同上 | **真缺口**：`:` 标签没有位置（§6） |
| `explicitVal = 1 : i64` | `SparseTensorAttrDefs.td:355` | **不是缺口**：`value ":" type(value)` 已可表达——值也是部件 |
| `qualified(type(x))` | `SparseTensorTypes.td:67` | **真缺口**：强制打印方言全名，是 `type(x)` 的变体 |
| `oilist` 语义 | `Operations.md:808` | **已修正**：每支 0/1、顺序无关，不是互斥 |
| 属性作为类型参数 | `SparseTensorTypes.td:49` | **待定**：`Parameters (encoding: Encoding)` 里 `Encoding` 是 `attr_def`；靠名字解析可区分，但语言没有"这是属性参数"的标记 |
| region 入口参数个数含算术 | `SparseTensorOps.td:1372` | **拒绝**：`dim_rank + 1` 是 verifier 计算，事实只收名字与整数 |
| 位集键与级的索引对应 | `SparseTensorAttrDefs.td:30` | **待定**：`[level_bitmask]` 记了形状，位↔级的语义留在 Doc |

## 6. 逃逸点账本：第一次告警

19 章的预算表在加入这两个方言后立刻告警：

| 指标 | 19 方言时 | 21 方言时 | 处置后 |
| --- | --- | --- | --- |
| 方言本地事实 | 8 | 34 | 19 |
| 单次使用 | 5 | 20 | 7 |
| 单次占比 | 62% | 59% | 37% |
| 跨 ≥3 方言未升级 | 0 | 1 | 0 |

两个方言把本地词表翻了四倍。**没有人偷懒**：每个方言都真心觉得"我这几个
性质特殊"——这正是逃逸点会变黑洞的机制。

按 19 章 R5 合并后是 19 个本地事实、7 个单次使用。这里的 15 个单次项已逐条
复审（§6.1）：8 个是同一根轴的重复写法，合并进核心词；7 个保留，因为语义
确实不可由现有词组合出来。

### 6.1 单次事实复审：8 合、7 留

| 原事实 | 处置 | 理由 |
| --- | --- | --- |
| `all_element_types_match(a, b)` | → `same_element_type` | tosa 已用同一个词的二元形式 |
| `element_type_is_result_type` | → `element_type_matches(range, result)` | 元素类型关系已有参数化写法 |
| `may_reallocate_buffer` | → `memory_effect(allocate)` | 重分配 = 分配 + 旧缓冲失效 |
| `resets_workspaces` | → `memory_effect(allocate)`+`write` | 分配并初始化 |
| `bridges_storage_to_value` | → `memory_effect(read)` | 从存储读回值 |
| `side_effects_only` | → `memory_effect(write)` | “无结果”是结构，副作用是写 |
| `writes_in_place`、`in_place_buffers` | → `in_place`（2 次） | 同一条轴：写回自身缓冲 |
| `at_least`、`same_size`、`same_type_operands`、`matches_input_arity`、`yields_to_enclosing_region`、`debug_only`、`first_attr_derived_result_type` | 保留 | 都是“带参数/另一根轴”的关系，没有核心词能表达 |

复审把单次占比从 59% 压到 37%，不靠删信息，靠换词。

## 7. 机械核对

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
| 语料解析 | 21/21 `parse OK` |
| `from`/`extends` 指向不存在的 schema | 0 |
| Assembly 引用未声明部件 | 0 |
| 声明后未使用的 schema/constraint/enum | 0 |
| C++ 泄漏 | 0 |
| 无文本通道的 op | 0 |
| 跨 ≥3 方言未升级的事实 | 0 |
| 外部类型名（逐条复核） | 6 个方言、11 个名字，均为可辨认领域词 |

## 8. 下一轮

1. 单次事实复审已完成（§6.1）；下一轮直接扩语料。
2. 候选方言：`llvmir`（类型异构最重）、`spirv`（432 个 def）、
   `arm_neon`/`arm_sve`（SIMD 形状族）、`openmp`/`openacc`（region 与子句多）。
3. 待补的真缺口：标签化结果组、`qualified(...)`、属性参数标记。

## 9. 结论

1. **92 个 op、0 条新产生式。** 属性层与类型层的厚，没有换来文法膨胀——
   因为 `type_def`/`attr_def` + `Assembly` + `Parameters` 已经是通用形状。
2. **读窄比写窄更危险。** `oilist` 的语法一直是对的，语义被读成"互斥"，
   于是语料把事实挪进注释——这种错误不会被 parse 抓到，只能靠对照上游文档。
3. **同名不同类型空间是真实需求**：`!emitc.opaque<...>` 与 `#emitc.opaque<...>`
   共享助记符、不共享命名空间。
4. **属性可以是数据结构，但不能变成自由文本。** `encoding` 的字典形状用
   "字符串键 + 部件值"就能表达；真正表达不了的是值内部的标签结构。
5. **预算第一次告警就来自 2 个方言。** 本地词表 8→34 说明"每个方言都觉得自己
   特殊"是默认趋势，规则的價值在于它先于失控发出信号。
