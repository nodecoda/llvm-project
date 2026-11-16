# rvcsr v1 语法规范

**状态**：frozen（v1，2026-11-11）
**机器可读版本**：[`../grammar/rvcsr.ebnf`](../grammar/rvcsr.ebnf)
**上游 SSOT**：`/home/ubuntu/admin/ana-docs/rvcsr-grammar-v1-2026-11-11.md`
**实现计划**：`.omx/plans/2026-11-11-rvcsr-standalone-dsl.md`（D2/D9 引用本规范）

本文件是 `rvcsr` 的 lexer / parser / sema 的语法合同。实现阶段不改语法，只实现；
若发现语法缺口，先改本文件（加领域构造或 `@` 逃逸点），再改 parser。**parser 不允许
做语法文件之外的隐式扩展。**

---

## 1. 一句话

`rvcsr` 是**声明式数据语言**，不是脚本语言：

- 没有函数、递归、模块、通用控制流；
- 只有“声明 + 有界宏 + 整数算术”；
- 594 条 CSR 记录不需要任何逃逸点；
- 逃逸点只处理“名字合成 / 顺序”这类语法缺口，不处理任意 C++。

---

## 2. 设计原则

| # | 原则 | 具体表现 |
| --- | --- | --- |
| P1 | 领域形状优先 | `csr` / `alias` / `deprecated` / `vendor` / `requires` 直接对应领域概念 |
| P2 | 非图灵完备 | 无函数、无递归、无 `if` 语句、无布尔表达式、无嵌套循环 |
| P3 | 宏必须有界 | `for` 只能整数升序、上限 1024；模板只能引用当前循环变量 |
| P4 | 零噪音 | 源文件里没有 `.`、没有 `class/def/let/foreach`、没有字段名列表 |
| P5 | 逃逸点显式 | 所有逃逸以 `@` 开头，默认拒绝；启用后计数并告警 |
| P6 | 失败要早 | 名字、feature、alias、encoding 的错都在 tblgen 阶段之前报出 |

---

## 3. 语言分层

```text
Layer 0  声明层      csr / alias / deprecated / vendor
Layer 1  有界宏层    for / ${i} / ${i:width} / + - ( )
Layer 2  条件层      requires / rv32only / rv32only if odd|even(i)
Layer 3  逃逸层      @literal / @priority   （默认关闭，--allow-escapes）
```

Layer 3 不是第四种语义，只是 Layer 0/2 的显式旁路；它不能引入代码、控制流或新的
求值模型。

---

## 4. 词法

```ebnf
ident        ::= ( letter | "_" ) { letter | digit | "_" } ;
letter       ::= "A".."Z" | "a".."z" ;
digit        ::= "0".."9" ;

integer      ::= decimal | hex ;
decimal      ::= digit { digit } ;
hex          ::= ( "0x" | "0X" ) hexdigit { hexdigit } ;
hexdigit     ::= digit | "A".."F" | "a".."f" ;

name_string  ::= '"' lower_ident '"' ;
lower_ident  ::= ( lower | "_" ) { lower | digit | "_" } ;
lower        ::= "a".."z" ;

comment      ::= "#" { any-char-except-newline } newline ;
```

- 空白（含换行）不参与语法；语句不写分号。
- `..` 与 `.` 由最长匹配区分；核心语法里只有 `..`，没有单点。
- 关键字（保留字）：
  `csr`、`alias`、`deprecated`、`of`、`vendor`、`requires`、`for`、`in`、
  `rv32only`、`if`、`odd`、`even`、`feature`、`experimental`、`extern`。
- `@` 是逃逸层保留 token；核心语法不出现。
- `ident` 词法允许大小写；**CSR 名 / vendor id 必须小写**由语义 E2010 检查；
  feature id 保留大小写。

---

## 5. 核心语法（Layer 0–2）

```ebnf
csr_file        ::= { top_item } EOF ;

top_item        ::= vendor_block | csr_decl | alias_decl | deprecated_decl | loop ;

vendor_block    ::= "vendor" ident [ "requires" feature_ref ]
                    "{" { vendor_item } "}" ;
vendor_item     ::= csr_decl | loop ;

loop            ::= "for" ident "in" integer ".." integer
                    "{" { csr_decl } "}" ;

csr_decl        ::= "csr" local_name "=" expr [ attr_block ] ;
alias_decl      ::= "alias" ident "of" expr ;
deprecated_decl ::= "deprecated" ident "of" expr ;

attr_block      ::= "{" attr { "," attr } "}" ;
attr            ::= "rv32only" [ "if" condition ]
                  | "requires" feature_ref ;
condition       ::= ( "odd" | "even" ) "(" ident ")" ;

local_name      ::= ident [ template [ ident ] ] ;
template        ::= "${" ident [ ":" integer ] "}" ;

expr            ::= term { ( "+" | "-" ) term } ;
term            ::= integer | ident | "(" expr ")" ;

feature_ref     ::= ident ;
```

### 5.1 产生式的领域理由

| 产生式 | 领域理由 |
| --- | --- |
| `csr` / `alias` / `deprecated` | 三种名字角色，直接用关键字表达，不再用两个 bool 推导 |
| `vendor ... { }` | vendor 是命名空间；`.` 是投影产物，源里不写 |
| `for i in a..b` | 20 个 TableGen `foreach` 的领域替身；不引入 `while` / 递归 |
| `local_name ::= ident [ template [ ident ] ]` | 语料只有 `prefix`、`prefix${i}`、`prefix${i}suffix` 三种 |
| `template ::= ${i[:width]}` | `hpmcounter${i}h`、`mclicilvl${i:2}`；没有任意格式化 |
| `expr ::= term { (+|-) term }` | 语料只用到 `0xc03 + (i - 3)`；没有位运算 / 乘除 |
| `rv32only if odd/even(i)` | `pmpcfg` 的 `!and(i,1)`；没有通用布尔表达式 |
| `requires` | 可用性条件是 feature 合取；LLVM 的 `FeatureBitset` 只能表达合取 |

### 5.2 示例

```text
# --- unprivileged ---
csr fflags = 0x001
csr frm    = 0x002
csr fcsr   = 0x003

# --- counters ---
for i in 3..31 {
  csr hpmcounter${i}    = 0xc03 + (i - 3)
  csr hpmcounter${i}h   = 0xc83 + (i - 3) { rv32only }
}

# --- aliases ---
csr tdata1 = 0x7a1
alias mcontrol     of 0x7a1
alias mcontrol6    of 0x7a1

# --- deprecated ---
csr stval = 0x143
deprecated sbadaddr of 0x143

# --- vendor namespace ---
vendor sf requires XSfmclic {
  csr mtvt = 0x307
}

# --- zero-padding ---
vendor qc requires Xqciint {
  for i in 0..31 {
    csr mclicilvl${i:2} = 0xbc0 + i
  }
}
```

投影后：

- `mtvt` → `sf.mtvt`
- `mclicilvl00` → `qc.mclicilvl00`
- `hpmcounter${i}h` → `hpmcounter3h`…

完整可解析样例见
[`../examples/riscv/riscvcsrs.rvcsr`](../examples/riscv/riscvcsrs.rvcsr)。

---

## 6. feature 文件（`*.rvfeat`）

```ebnf
feature_file  ::= { feature_decl } EOF ;

feature_decl  ::= "feature" ident
                  [ "{" [ feature_attr { "," feature_attr } ] "}" ]
                | "extern" "feature" ident ;

feature_attr  ::= "rv32only" | "experimental" | "requires" ident ;
```

```text
extern feature F

feature XSfmclic { experimental }
feature XSfsclic { experimental }
feature Xqciint  { rv32only }
feature Xqccmi   { rv32only }
feature XAIFET   { requires F }
```

- `extern feature F` 是**核心构造**，不是逃逸：允许引用标准扩展而不建模它。
- feature 的 `requires` 只记录直接依赖，不展开闭包（v1 生成表只写直接依赖）。
- feature id 大小写敏感；重复声明 E2016。

---

## 7. 静态语义

| ID | 规则 | 级别 |
| --- | --- | --- |
| C1 | CSR 名 / vendor id 必须小写 | E2010 |
| C2 | 名字大小写不敏感唯一 | E2002 |
| C3 | alias / deprecated 必须命中恰好一个 canonical | E2003（无）/ E2014（多个） |
| C4 | alias / deprecated 只能顶层，不能带属性 | 语法 |
| C5 | `requires` 必须已声明（含 `extern feature`） | E2005 |
| C6 | encoding 求值后 0..0xFFF | E2001 |
| C7 | 同 encoding 的**无条件** canonical ≤ 1 | E2006 |
| C8 | `enum_name`（`.`→`_`）不重复 | E2007 |
| C9 | 模板 / 条件里的循环变量必须绑定 | E2008 |
| C10 | `rv32only if` 只能是 `odd/even(<loop-var>)` | E2009 |
| C11 | 循环上下界升序、迭代 ≤ 1024；模板宽度 1..9 | E2011 |
| C12 | 同一属性不得重复；同一 feature 的 `requires` 不得重复 | E2012 |
| C13 | `effective_rv32only = explicit or any(required feature.rv32only)` | 派生 |
| C14 | vendor 块没有 `requires` | W1003 |
| C15 | `*h` 未标 rv32only | W1001 |
| C16 | rv32only 但不以 `h` 结尾且非 pmpcfg / qc | W1002 |
| C17 | 使用逃逸点但未开 `--allow-escapes` | E3001 |
| C18 | 使用逃逸点（已开启） | W2001 + 计数 |

`C13` 的意义：`Xqciint` / `Xqccmi` 在 feature 文件里已标 `rv32only`，64 条 `qc.*`
不必逐条写 `rv32only`，由编译器派生。`pmpcfg` 奇数项、`*h` 这类没有 feature 的
情况仍显式写。

其他模型层约定（不是语法，但实现必须一致）：

- **唯一键是 name**，encoding 是 1:N（25 组多名字，最大 7 个共享 `0x7A1`）。
- `requires` 构成有限 DAG；sema 用传递闭包做可达性 / 死记录检查，
  但**生成表只写直接依赖**，与 LLVM `haveRequiredFeatures` 的合取语义一致。
- v1 只消费 feature 的两个属性：`rv32only`、`experimental`。

---

## 8. 冻结规则（v1 不得协商）

1. **源语法没有 `.`**。vendor 是作用域，投影时才生成 `sf.` / `qc.` / `aif.` 前缀。
2. 模板只有 `${i}` / `${i:width}`；`${i:2}` = 最小宽度 2、补零；宽度范围 1..9。
3. alias / deprecated 只能顶层、无属性，继承 canonical 的可用性。
4. vendor 块 `requires` 可选（块级糖，可被块内 `requires` 覆盖/补充）；
   `rv32only` 可由 feature 派生。
5. 循环必须带 `{}`；循环体只能 `csr_decl`，**不嵌套**；vendor 不嵌套。
6. 表达式只有 `+` `-` `(` `)`；循环边界只能是整数字面量。
7. 当前 594 条语料 **零逃逸点**；CMake/CI 门禁要求 `--stats` 的 `escapes <= 0`。

---

## 9. 逃逸点（Layer 3，默认关闭）

```ebnf
csr_name_escaped ::= local_name | "@literal" name_string ;
attr_escaped     ::= attr | "@priority" integer ;
```

```text
# 名字合成缺口：模板表达不了时，给一个精确的 local name
vendor foo requires F {
  csr @literal "bar_baz" = 0x123
}

# 顺序缺口：同 encoding 的打印优先级（仅 canonical）
csr mbusaddr = 0x7d5 { requires XAIFET, @priority 10 }
```

语义：

- `@literal`：只允许小写 local name（无点）；vendor 前缀仍由作用域添加；
  与其他名字一样参与唯一性检查。
- `@priority`：仅 canonical；默认 0，数值大者优先；用于 `selectDisplayName` /
  表排序。
- 所有逃逸：以 `@` 开头、默认拒绝（E3001）、启用后每条 W2001 并计入 `--stats`、
  CI 可设预算（v1 预算 = 0）、只影响一个声明 / 一个属性。

不提供的逃逸：任意 C++ 表达式、OR/NOT feature 逻辑、任意字符串格式化、
`#include` / 宏 / 函数、直接注入生成代码。

---

## 10. 为什么它不是图灵完备（可证明）

命题：任意 `rvcsr` 程序的求值在 `O(D × 1024)` 步内停机，`D` 为顶层声明数。

1. 唯一的重复构造是 `for i in a..b`，而 `a`、`b` 的语法是 `integer`——
   字面量，不能引用变量、不能是表达式、不能是另一次循环的结果。因此迭代次数在
   解析期即确定，且 P3 限制 `b - a + 1 ≤ 1024`。
2. 嵌套循环被语法禁止（`loop` 体内只有 `csr_decl`），故总展开数 `≤ 1024 × D`。
3. 其余构造都是单记录上的有界计算：
   - `expr` 只有 `+ - ()`，无函数调用、无递归定义，项数由源码长度决定；
   - `template` 只做一次整数→补零字符串；
   - `rv32only if odd|even(i)` 是纯谓词，不是分支求值。
4. 没有可变状态、没有引用、没有 I/O、没有不动点计算。

结论：语言是“有限折叠”（finite fold over a bounded record set）。
任何需要“算到收敛”的需求（feature 依赖闭包、任意格式化、条件别名、生成代码）
都不属于这门语言——要么进逃逸点，要么留在后端 emitter。

---

## 11. 明确拒绝的特性

| 特性 | 状态 | 理由 |
| --- | --- | --- |
| `class` / `def` / `let` / `multiclass` | 拒绝 | TableGen 元语言，不是领域词 |
| 函数 / 递归 / 宏定义 | 拒绝 | 非图灵完备 |
| `if` 语句 / 布尔表达式 | 拒绝 | 只有 `rv32only if odd/even(i)` 一个受限条件 |
| 嵌套 `for` / 嵌套 `vendor` | 拒绝 | 语料不需要；名字模板只允许一个循环变量 |
| 通用表达式（乘、除、位运算、移位） | 拒绝（v1） | 语料只用 `+ - ()`；需要时按领域扩展 |
| `include` / `import` | 拒绝（v1） | 594 条单文件足够；组合需求出现再加 |
| 任意 C++ / `code` 字段 | 拒绝 | 这是要消灭的“神秘感”来源 |
| OR/NOT feature 逻辑 | 拒绝 | 目标 `FeatureBitset` 表达不了 |
| 任意字符串格式化 | 拒绝 | 只保留最小宽度；否则用 `@literal` |
| 运行时逻辑 / 生成代码注入 | 拒绝 | DSL 只描述数据 |

---

## 12. 冻结判据（实现验收时逐条对照）

1. 完整语料展开：594 / 563 / 25 / 7 / 9 / 3 / 128 / 198。
2. `qc.mclicilvl00..31` 精确。
3. 源文件无单点；投影后只有 `aif.` / `qc.` / `sf.`。
4. 六组 alias / deprecated 的 canonical 精确。
5. 每条 E/W 规则有夹具；E3001 默认拒绝逃逸。
6. 连续两次生成 `sha256` 相同。
7. `llvm-diff.sh` 记录级 0 差异。
8. standalone 空目录构建通过。
9. 当前语料逃逸计数 = 0。

---

## 13. 冻结后改动规则

1. 语法只允许在**发现领域构造缺口**时改，且必须附语料证据。
2. 不允许在 parser 里做“顺手支持”的隐式扩展；语法文件没写的，parser 不认。
3. 每次改动递增版本号（v1 → v2），并保留旧版本的语料回归结果。
4. 本文件与 `../grammar/rvcsr.ebnf` 必须同步修改，二者不一致时以本节规则为准并
   立即修复。
