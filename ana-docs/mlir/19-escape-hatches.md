# 19. 逃逸点：五种通道与它们的约束

> 配套：[`dialect-grammar.y`](dialect-grammar.y)（`Assembly Generic;`）、
> [`tools/dialect-corpus.py`](tools/dialect-corpus.py)（`escape points` /
> `text channels` / `fact vocabulary` / `external type names` 四段报告）。
> 语料：`corpus/*.dialect`（21 个方言、438 个 op、4735 行）。

## 0. 为什么单写一章

一门 DSL 的能力由两件事决定：**它能直接说什么**，和**它说不出来时往哪儿放**。
只做前者，语言会在真实语料前不断破例；只做后者，语言会退化成"标签 + 自由文本"。

18 章已经出现过三种逃逸：`Assembly Custom;`、绑定与 region 的事实注解、
`Implements [...]`。但那时它们只是"能用的地方"，没有边界、没有预算、没有检查。
这一章把它们变成受约束的机制。

三条要求，缺一不可：

| 要求 | 反例（会变成什么） |
| --- | --- |
| **显式**：必须写出来才能用 | 省略 Assembly 子句被当成 generic → 忘记写和故意不写无法区分 |
| **单维**：只覆盖它声明的那一维 | 用 `Assembly Custom;` 顺手把"有几个 region"也藏起来 → 结构不可枚举 |
| **可数**：工具能统计出用了多少 | 事实名随手造 → 词表膨胀成注释的替代品 |

## 1. 五种通道

| # | 通道 | 语法 | 覆盖 | 明确不覆盖 |
| --- | --- | --- | --- | --- |
| 1 | 块格式 | `Assembly { ... }` | 部件的**文本顺序** | 结构（部件照样要在子句里声明） |
| 2 | 局部钩子 | `custom<Hook>(a, type(b))` | 一段子格式 | 实参以外的部件；钩子名不是引用 |
| 3 | 整体钩子 | `Assembly Custom;` | 该 op/类型的**全部文本** | 结构、语义（事实仍要写） |
| 4 | 通用格式 | `Assembly Generic;` | 无自定义文本（generic） | 同上 |
| 5 | 事实 | `Properties [..]`、绑定/region 注解、`Implements [..]` | 关于已声明部件的**性质** | 位置、结构、新的可枚举对象 |

第 3、4 条容易混：`Custom` 表示"有手写打印器"，`Generic` 表示"没有自定义格式"。
两者都不写 = 语法错误（本轮之前是"等于 Generic"，见 R1）。

另有一类不是通道、但同样开放的引用：**类型名**。`Operands (x: 某个名字)` 里的
名字若既不在本语料声明、也不是内建类型，就是一个开放的逃逸点（§2.7）。

## 2. 七条规则

### R1 文本通道必须显式

```text
Assembly { ... }      /* 块格式 */
Assembly Custom;      /* 手写打印器 */
Assembly Generic;     /* 没有自定义格式 */
```

**不允许省略。** 省略是看不见的逃逸：读者无法区分"这个 op 就是通用格式"与
"作者还没写"。文法代价是一条产生式和一个 token：

```yacc
assembly
  : L_BRACE asm_list R_BRACE
  | KW_CUSTOM SEMI
  | KW_GENERIC SEMI
  ;
```

见证：`shape.debug_print`（`ShapeOps.td:691`）、`shape.split_at`（`:703`），
以及 `emitc.constant`/`variable`/`member`/`member_of_ptr`（无 `assemblyFormat`
也无 `hasCustomAssemblyFormat`）。

### R2 逃逸点只覆盖它声明的那一维

- `Custom`/`Generic` 只覆盖**文本**。结构仍在子句里：`quant.uniform` 即使文本
  由钩子定义，`Parameters` 也必须逐项列出。
- `custom<Hook>(args)` 只覆盖**实参列出的部件**的打印，不得引入新部件。
  这由 dangling 检查保证：钩子实参必须能在本 op 或父 schema 里解析到。
- 事实只覆盖**性质**，不得重复子句已经说过的结构。已识别的三个违规：

| 违规事实 | 位置 | 与什么重复 | 处置 |
| --- | --- | --- | --- |
| `function_interface` | `corpus/shape.dialect` | `Implements [FunctionOpInterface]` | 删除 |
| `loop_like` | `corpus/tosa.dialect` | `Implements [LoopLikeOpInterface]` | 删除 |
| `single_block_implicit_terminator(Y)` | tosa 两处 | `single_block` + `implicit_terminator(Y)` | 拆成两个事实 |

第三条的教训：**组合优于新词**。两个既有事实的合取不需要第三个名字。

### R3 逃逸点必须可数

```text
## escape points
| file | Assembly Custom | generic | custom<> | facts | distinct |

## text channels
| file | block | Custom | Generic |
ops without a text channel: none

## fact vocabulary: core=39 used, local(distinct)=19, local(single-use)=7
```

`ops without a text channel: none` 是 R1 的自动检查。`distinct/facts` 接近 1
说明每个事实只用一次——那不是事实，是注释。

### R4 新增事实、新增产生式，还是走逃逸通道

判据是**这个新东西有没有位置**：

| 新东西是 | 处置 | 例 |
| --- | --- | --- |
| 有固定语法位置、可嵌套、可被 Assembly 引用 | **新增产生式** | `type_arg : INT`（秩在类型里有位置） |
| 关于已存在部件的性质 | **新增事实** | `[cardinality(4)]`、`positive`、`entry_args_unwrap(x)` |
| 纯文本、不可分析 | **逃逸通道** | `Assembly Custom;`（quant 的 `uniform`）、`custom<Hook>` |

反例检查：`ref(type($x))` 有位置（在 custom 实参里），所以它**不能用事实代替**，
只能承认是缺口——见 §5。

### R5 词表受管

事实分两层：

- **核心词表**（39 个，§3）：语义在本章定义，任何方言可用，不需要再解释。
- **方言本地事实**（19 个，§4）：只在一个方言出现，必须在该方言文件里被解释。

升级与合并规则：

1. 同一概念已有核心词 → **用核心词**，不得造近义词。
   本轮合并：`recursive_memory_effects`→`recursive_effects`、
   `allocates_memory`/`reads_memory`/`writes_memory`/`reads_and_writes_memory`
   →`memory_effect(read|write|allocate)`。
2. 两个核心事的合取 → **写两个事实**，不得造组合词
   （`single_block_implicit_terminator`、`reads_and_writes_memory`）。
3. 同一根轴只留一种写法：块数是 `single_block` / `min_blocks(n)` /
   `max_blocks(n)`；`at_most_one_block`、`zero_or_one_block` 都归到
   `max_blocks(1)`。
4. 新事实要在 ≥2 个方言有独立见证，才能进核心表；工具会报
   "local but used by >=3 dialects (must promote)"。

### R6 预算

| 指标 | 当前值 | 红线 | 超线的含义 |
| --- | --- | --- | --- |
| `ops without a text channel` | 0 | 0 | 有 op 没交代文本，语料不完整 |
| `Assembly Custom` 占 op 比 | 68/438 = 16% | 25% | 过高说明块格式表达力不足 |
| 单次使用的本地事实 | 7/19 = 37% | 50% | 事实在替代注释 |
| 跨 ≥3 方言的非核心事实 | 0 | 0 | 词表失控 |

预算的作用不是卡数字，而是**在扩语料前先怀疑构造**：一个方言里
`Assembly Custom` 占比异常高，先问"是不是缺了某个块格式构造"。

### R7 类型名也是逃逸点

`Operands (x: token)` 里的 `token` 若是拼错的 `tokn`，语法完全合法。
所以类型引用必须可解析：**本语料声明 > 内建 > 外部署名（逐条列出）**。

工具报告：

```text
## external type names (neither declared in corpus nor builtin)
| gpu.dialect | address_space, gpu_handler, gpu_target |
| tosa.dialect | scalar |
```

这条检查第一次跑就抓到了真错误：`async` 的类型声明改名成 `coro.id` 后，
引用处仍写着 `coro_id`——语法合法、语义悬空。外部署名允许存在，但必须
是**可辨认的领域词**，拼写错误无处藏身。

## 3. 第一次预算告警（本轮实证）

加入 `emitc`（55 op）与 `sparse_tensor`（39 op）后：

| 指标 | 加之前（19 方言） | 加之后（21 方言） | 处置后 |
| --- | --- | --- | --- |
| 方言本地事实 | 8 | **34** | 19 |
| 单次使用 | 5 | **20** | 7 |
| 单次占比 | 62% | **59%** | 37% |
| 跨 ≥3 方言未升级 | 0 | 1（`operand_segments_by_attribute`） | 0 |

两个方言就把本地词表翻了四倍。这正是"逃逸点会变黑洞"的现场：不是有人故意
偷懒，而是**每个方言都觉得自己那点性质特殊**。

按 R5 处理后的结果：

| 合并前 | 合并后 | 依据 |
| --- | --- | --- |
| `at_most_one_block`、`zero_or_one_block` | `max_blocks(1)` | 与 `min_blocks(n)` 同一根轴 |
| `allocates_memory`/`reads_memory`/`writes_memory`/`reads_and_writes_memory` | `memory_effect(allocate/read/write)` | 一个概念一个词，合取写两次 |
| `operand_segments_sized_by_attribute` | `operand_segments_by_attribute` | 与 2 个方言的既有写法重复（升级为核心词） |
| `recursive_memory_effects` | `recursive_effects` | 同一 trait 家族两种拼法 |
| `all_element_types_match(a, b)` | `same_element_type` | tosa 已有同义词，二元写法多余 |
| `element_type_is_result_type` | `element_type_matches(range, result)` | 元素类型关系已有参数化形式 |
| `may_reallocate_buffer` | `memory_effect(allocate)` | 重新分配就是分配 + 旧缓冲失效 |
| `resets_workspaces` | `memory_effect(allocate)` + `memory_effect(write)` | 分配并初始化工作区 |
| `bridges_storage_to_value` | `memory_effect(read)` | 从存储读回值 |
| `side_effects_only` | `memory_effect(write)` | “无结果”是结构，副作用是写 |
| `writes_in_place`、`in_place_buffers` | `in_place` | 同一条轴：写回自身缓冲 |

合并后 19 个本地事实、7 个单次使用（37%），回到红线内；剩下 7 个各有
不可替代的理由（§5）。

## 4. 核心词表（39）

| 组 | 事实 | 含义 |
| --- | --- | --- |
| 结构/trait | `pure` `commutative` `terminator` `elementwise` `constant_like` | 与 ODS trait 对应的领域性质 |
| | `symbol` `symbol_table` `symbol_visibility` `isolated_from_above` | 符号与作用域 |
| | `recursive_effects` `no_memory_effect` `memory_effect(x)` | 副作用；`x ∈ {read, write, allocate}` |
| | `automatic_allocation_scope` `destination_style` `contraction` | 分配与角色约定 |
| | `return_like` `no_region_args` `affine_scope` `infer_tensor_type` | 其它结构与作用域性质 |
| | `same_variadic_operand_size` `operand_segments_by_attribute` | 变长部件的分段 |
| region | `single_block` `min_blocks(n)` `max_blocks(n)` | 块数（恰好 / 至少 / 至多） |
| | `implicit_terminator(op)` `no_terminator` | 终结符 |
| 绑定 | `positive` `non_negative` `cardinality(n)` `arity(parts, n)` | 值域与元数 |
| 关系 | `has_parent(op)` | 唯一允许的父 op 约束（`parent_one_of` 已删） |
| | `same_shape` `same_rank` `same_element_type` `broadcastable` | 形状关系 |
| | `element_type_matches(a, b)` `unwraps_element_type(x)` | 元素类型关系 |
| | `entry_args_unwrap(x)` `yield_types_match(x)` | region 与部件的关系 |

四个"绑定事实"约束的是**部件**而不是 op，所以写在绑定注解里
（16 章移 region 约束、18 章加属性值域，同一条原则）：

```text
Attributes (pad: array<i64> [cardinality(4)]);          /* 长度 */
Attributes (count: i64 [positive]);                     /* 值域 */
Operands   (operands: variadic<any> [arity(inputs, 2)]);/* 元数 */
```

## 5. 方言本地事实（19）

每个都必须在所属方言里被解释；跨方言借用要先升级为核心词。

| 事实 | 方言 | 次数 | 需要解释的内容 |
| --- | --- | --- | --- |
| `c_expression` | emitc | 14 | 该 op 的值是 C 表达式，可参与 C 侧求值 |
| `entry_args` | sparse_tensor | 5 | region 入口块参数由操作数给出 |
| `resolvable_shape_operands` | tosa | 5 | 形状操作数必须能在常量表里解析 |
| `always_inline` | emitc | 4 | 生成的 C 函数体必须内联 |
| `default_dialect` | emitc | 3 | 文件体内可省略该方言前缀 |
| `level_bitmask` | sparse_tensor | 3 | 维/lvl 的位掩码关系 |
| `pure_if_fundamental` | emitc | 3 | 仅当元素类型是基础类型时无副作用 |
| `graph_region` | emitc | 2 | region 是表达式图而非 CFG |
| `in_place` | sparse_tensor | 2 | 写回自身缓冲 / 原地排序；不是“分配”也不是“纯读” |
| `same_operands_and_result_type` | sparse_tensor | 2 | 操作数与结果同元素类型 |
| `shape_operator` | tosa | 2 | 该 op 属于形状算子族 |
| `speculatable` | sparse_tensor | 2 | 无副作用且可安全推测执行 |
| `at_least` | sparse_tensor | 1 | 值下限依赖于另一个参数（`m >= n`），非固定常数 |
| `debug_only` | sparse_tensor | 1 | 仅调试用途，可被删除 |
| `first_attr_derived_result_type` | tosa | 1 | 结果类型由属性导出，不是由操作数推导 |
| `matches_input_arity` | pdl_interp | 1 | 结果元数等于输入元数（符号元数，非常数） |
| `same_size` | sparse_tensor | 1 | 两个 **变长部件** 等长（跨结果/操作数，不是元素类型） |
| `same_type_operands` | pdl_interp | 1 | 句柄类型相等（不是元素类型相等） |
| `yields_to_enclosing_region` | sparse_tensor | 1 | 该终结符把操作数交给外层 region |

## 6. 没有通道的缺口（诚实清单）

有些缺口**不能用事实补**，因为它们是位置或结构，不是性质：

| 缺口 | 见证 | 为什么事实不行 |
| --- | --- | --- |
| `ref(type($x))` | `PDLInterpOps.td:1043` | 它是 custom 实参的一个**形态**，事实无法改变实参语法 |
| 匿名位置部件 | `AsyncOps.td:223` | "有没有名字"是结构；事实只能描述有名字的部件 |
| `type_def` 的底层类型 | `ShapeBase.td:94` | "等价于 rank-1 index 张量"是类型等式，不是谓词 |
| 接口方法覆写 | `ShapeOps.td:130` | 覆写列表是 `Implements` 的实参形态 |
| `oilist<sep>` 的分隔符 | `EmitC.td` 的 `arg_attrs`/`res_attrs` | 分隔符是文本结构，当前 `oilist` 不接受 `<sep>` 实参 |

承认缺口比用逃逸点伪装完整更好。每个缺口都写明了见证与影响面，下一轮
要么补产生式，要么永久拒绝。

## 7. 结论

1. **逃逸点是对表达力的借贷，不是赠予。** 借来的必须记账（显式）、限定用途
   （单维）、能查余额（可数）。
2. **省略是最坏的逃逸形式**，因为它不可见。`Assembly Generic;` 多一个 token，
   换来"每个 op 都交代了文本从哪来"。
3. **事实的诱惑最大，因为最自由。** 39 个核心词 + 19 个本地词的上限，靠三条
   纪律维持：同一概念一个词、组合不造新词、跨方言借用要升级。
4. **预算第一次告警就来自两个方言**：本地词表 8→34、单次使用 5→20。
   这不是意外，是默认趋势；规则的价值在于它先于失控被发现。
5. **类型名是最隐蔽的逃逸点**：拼错也合法。所以它必须被逐条报告——
   这条检查第一次跑就抓到 `coro_id` 悬空。
