# 17. 第三轮语料：memref / vector / affine / math / index

> 配套文件：[`corpus/memref.dialect`](corpus/memref.dialect)、
> [`corpus/vector.dialect`](corpus/vector.dialect)、
> [`corpus/affine.dialect`](corpus/affine.dialect)、
> [`corpus/math.dialect`](corpus/math.dialect)、
> [`corpus/index.dialect`](corpus/index.dialect)
> （合计 657 行、80 个 op）；
> [`dialect-grammar.y`](dialect-grammar.y)（**56 条规则 / 130 条产生式**，0 冲突）；
> 14 个语料文件全部 `parse OK`。
>
> 与 16 章的关系：16 章用 3 个方言换来 10 项扩展；本章用 5 个形状差异更大的
> 方言**检验文法是否收敛**。

## 0. 方法：这一轮的目标是"不动文法"

前两轮的扩展都由新方言逼出来。本轮反过来验证归纳质量：挑 5 个与已有方言
形状差异大的领域（内存、向量、仿射、数学函数、index 算术），如果文法
几乎不用改，说明前两轮抽象抓住的是领域共性，而不是 arith/gpu/linalg 的偶然。

结果是：**只改了 1 处文法**（`custom` 的实参可以是 `type`），其余 9 类
新构造全部被已有产生式接住。

## 1. 语料

| 文件 | 行数 | dialect | constraint | enum | schema | op |
| --- | --- | --- | --- | --- | --- | --- |
| `memref.dialect` | 192 | 1 | | 1 | | 18 |
| `vector.dialect` | 207 | 1 | | 2 | | 20 |
| `affine.dialect` | 113 | 1 | | | | 11 |
| `math.dialect` | 66 | 1 | 1 | | 4 | 15 |
| `index.dialect` | 79 | 1 | | 1 | 1 | 16 |
| 其余 9 个方言 | 1152 | 9 | 7 | 10 | 14 | 119 |
| **合计** | **1809** | **14** | **8** | **14** | **19** | **199** |

各方言的领域贡献：

- **memref**：分配（`alloc`/`alloca`/`alloca_scope`）、访问（`load`/`store`）、
  视图（`subview`/`reinterpret_cast`/`view`）、符号（`global`/`get_global`）、
  原子（`atomic_rmw`）。`load`/`store` 的可选修饰是互斥组。
- **vector**：内存传输（`transfer_read`/`transfer_write`）、访问（`load`/
  `store`/`maskedload`/`maskedstore`）、逐元素（`broadcast`/`fma`/`bitcast`/
  `interleave`/`deinterleave`）、位置（`extract`/`insert`）、规约
  （`reduction`/`contraction`）、掩码作用域（`mask`）、调试（`print`）。
- **affine**：`apply`/`min`/`max` 是纯映射；`for`/`parallel` 是带
  `affine_map` 边界的循环；`if` 的两个 region 约束不同；`load`/`store`/
  `prefetch` 是仿射下标访问；`delinearize_index` 用局部格式钩子。
- **math**：15 个 op 只有四种形状（一元/二元/三元/整数二元），全部由 4 个
  schema 提供；`fastmath`/`roundingmode` 复用 arith 的枚举。
- **index**：所有 operand/result 都是 `index`，所以文本里没有类型；10 个
  二元 op 共用一个 schema，`cmp` 只有谓词 + 两个 operand。

## 2. 新构造：9 类被接住，1 类需要改文法

| # | 构造 | 见证 | 文法动作 |
| --- | --- | --- | --- |
| 1 | `custom` 实参可以叫 `type` | `memref.global` 的 `$type` | **新增 `custom_arg : IDENT \| A_TYPE`** |
| 2 | 同一 op 内混合 region 约束 | `affine.if`：then `[single_block]`，else 任意 | 无（16 章的 region 注解已够） |
| 3 | `oilist` 分支没有前缀字面量 | `vector.print`：`str $s \| punctuation $p` | 无（分支就是 `asm_list`） |
| 4 | `oilist` 分支里嵌 `custom` | `memref.load`/`store` | 无 |
| 5 | `oilist` 分支用 `=` 而不是括号 | `vector.load`/`store` | 无 |
| 6 | `bool` 属性带 `= false` 默认值 | `memref.load.nontemporal` 等 | 无（`expr : IDENT \| INT`） |
| 7 | schema 名字带方言前缀 | `math.Unary`、`index.Binary` | 无（名字允许 `.`） |
| 8 | 跨方言枚举复用 | math 用 arith 的 `FastMathFlags`/`RoundingMode` | 无 |
| 9 | 关键字相邻的 op 名 | `affine.for`、`affine.if`、`index.and/or/xor` | 无（软关键字） |
| 10 | 枚举 case 名与拼写不同 | `memref.atomic_rmw` 的 `addf` | 无（16 章前已有 `CmpFPredicate`） |

## 3. 三个值得单独记的验证点

### 3.1 `affine.if`：混合 region 约束

```text
Regions (then_region: region [single_block], else_region: region);
```

这是第一个**同一个 op 里两个 region 约束不同**的真实例子
（`AffineOps.td:433`：`SizedRegion<1>:$thenRegion, AnyRegion:$elseRegion`）。
16 章把 `single_block` 从 op 属性搬到 region 绑定时还没有这样的见证；
本轮它证明那次搬家不是过度设计：op 级属性无法表达"then 恰好一块、else 不限"。

### 3.2 `vector.print`：oilist 分支不必以字面量开头

```text
Assembly { if source { source ":" type(source) }
           oilist("str" string_literal | "punctuation" punctuation)
           attr-dict }
```

两个分支都以字面量开头（`"str"`/`"punctuation"`），但**分支本身是
`asm_list`**，允许任意元素序列——语法不需要为 "oilist 分支" 再引入一层
特殊规则。

### 3.3 丢弃 custom 的 C++ 配置实参

`affine.delinearize_index` 在 ODS 里的钩子是：

```td
custom<DynamicIndexList>($dynamic_basis, $static_basis, "{}",
                         "::mlir::AsmParser::Delimiter::Paren")
```

后两个实参是 C++ 配置（默认值字面量、分隔符枚举），不是领域事实。语料里
只保留部件实参：

```text
custom<DynamicIndexList>(dynamic_basis, static_basis)
```

`dialect-corpus.py` 的 C++ 泄漏检查（`::`、`namespace`、`#include`、`$x`、
`[{`）会抓住任何试图照抄它们的行为。这是 14 章"定义 vs 生成"边界在
partial custom 上的具体应用。

## 4. `type` 作为 custom 实参

`memref.global` 的钩子是 `custom<GlobalMemrefOpTypeAndInitialValue>($type,
$initial_value)`（`MemRefOps.td:1244`）。`type` 在 Assembly 模式里是关键字
（`type($x)` 打印类型），但这里它是**部件名**，后面跟 `,` 而不是 `(`。

LR(1) 用 lookahead 区分两者：

```yacc
custom_arg
  : IDENT
  | A_TYPE      /* type, 而不是 type( */
  ;
```

不需要新的词法模式，也不需要在语料里改名。这是 16 章软关键字思路在
Assembly 模式里的对应物：**关键字只在它真正起语法作用的位置是关键字**。

## 5. 跨方言复用

`math` 的 `fastmath`/`roundingmode` 直接引用 arith 的枚举：

```text
Attributes (fastmath: FastMathFlags = none);
Attributes (roundingmode: optional<RoundingMode>);
```

这不是名字冲突，而是领域事实：math op 的 fastmath 就是 arith 的
`FastMathFlags`。工具的死声明检查是全局的，因此 arith 的枚举因为被 math
使用而仍然"活着"。

schema 名字则用方言前缀（`math.Unary`、`index.Binary`），因为 schema 是
**方言本地**的：`arith.IntBinary` 与 `math.IntBinary` 不是同一个东西。名字
允许 `.`，所以不需要额外的命名空间语法。

## 6. 文法增量

| 指标 | 16 章 | 17 章 |
| --- | --- | --- |
| 规则 | 55 | **56** |
| 产生式 | 128 | **130** |
| 冲突 | 0 | **0** |
| 方言 / 语料行 | 9 / 1152 | **14 / 1809** |
| op | 119 | **199** |
| 解析通过 | 9/9 | **14/14** |

新增规则 1 个：`custom_arg`（`custom_arg_list` 从"IDENT 列表"改为
"custom_arg 列表"）。

## 7. 见证表

| 构造 | 见证 | 计数 |
| --- | --- | --- |
| `custom_arg` 的 `A_TYPE` 分支 | `memref.global` | 1 |
| `oilist`（累计） | gpu.barrier、memref.load/store、vector.load/store/print | 6 |
| `custom<Hook>`（累计） | tensor、memref、vector、affine | 23 |
| `functional-type`（累计） | gpu.all_reduce/subgroup_reduce、tensor.reshape | 3 |
| `single_block`（累计） | scf、gpu、linalg、tensor、memref、vector、affine | 24 |
| `has_parent`（累计） | scf、func、gpu、tensor、memref、vector | 16 |

工具统计（14 个文件）：

```text
assembly: type=124, attr-dict=97, if=53, custom=23, operands=7,
          oilist=6, results=5, functional-type=3, attr-dict-with-keyword=1
region facts: single_block=24
type ctors: variadic=115, optional=77, array=45
facts: has_parent=16
```

## 8. 机械核对

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
| 语料解析 | 14/14 `parse OK` |
| `from`/`extends` 指向不存在的 schema | 0 |
| Assembly 引用未声明部件 | 0 |
| 声明后未使用的 schema/constraint/enum | 0 |
| C++ 泄漏（`::`、`namespace`、`#include`、`$x`、`[{`） | 0 |

## 9. 局限与下一轮

1. **`memref.prefetch` / `affine.prefetch` 用 `Custom`。** 它们的文本形式是
   一串 `is_write`/`locality`/`cache_type` 关键字；语料先记结构，等需要
   精确文本时再写块格式。
2. **`vector.print` 的 `punctuation` 默认值只记录为 `= newline`。** 默认值
   的打印行为属于生成细节，不在领域层。
3. **`affine.if` 的 `condition` 是 `affine_set` 属性。** 整数集本身没有
   语法（与 01–09 章的 parser 分析一致：affine 有自己的子语言）。
4. **没有 `MinSizedRegion`/`MaxSizedRegion`。** pdl_interp 的
   `MinSizedRegion<1>` 是下一轮的候选。
5. **没有 attribute-heavy 方言。** stablehlo/tosa 的 op 大量使用
   `array<...>`/`dict`，下一轮可以检验属性层。

下一轮语料：`pdl_interp`（最小块数）、`stablehlo` 或 `tosa`（属性密集）、
`vector` 的 `shuffle`/`scalable` 变体、`memref.dma_start`。

## 10. 结论

1. **收敛的证据是"新方言不需要新产生式"。** 5 个方言、80 个 op 只换来
   1 条新规则；前两轮的抽象（列表/`|`、region 注解、`oilist`、`custom`、
   软关键字）覆盖了新领域的 9 类构造。
2. **per-region 约束从"设计上更对"变成"被真实 op 需要"。** `affine.if` 的
   then/else 是第一个混合约束见证。
3. **边界靠检查器守住。** custom 的 C++ 配置实参被主动丢弃；如果照抄，
   泄漏检查会立刻变红。
4. **方言本地身份用 `.` 表达。** `math.Unary`、`index.Binary` 让 schema
   不必全局唯一；跨方言复用的枚举则保持裸名，因为它们是同一领域对象。
5. **下一步的收益在语料密度，不在文法。** 剩余 36 个方言中，多数差异在
   属性形状与 assembly 细节；文法应继续少动，工具应继续加强审计。
