# 13. ODS 语义文法：把寄生在 TableGen 里的领域语言写出来

> 配套文件：
> [`ods.y`](ods.y)（记录层文法）、[`ods-asm-format.y`](ods-asm-format.y)
> （`assemblyFormat` 子语言文法）。
>
> 两份文件都是 bison 3.8.2 接受、**0 冲突**的 yacc 文法，但**没有任何 parser
> 执行它们**——这正是本章的论点。

## 0. 这份文法是什么，不是什么

ODS（Operation Definition Specification）不是一门独立实现的语言，它是**寄生在
TableGen 语法结构上的语义层**。TD 的 parser 只看到 `def` / `class` / `let` /
标识符 / 字符串；"这是一个 Op"、"这个 dag 是 `arguments`"、"这个 `class` 是类型
约束"这些判断，发生在 `mlir-tblgen` 遍历 `Record` 的时候。

| 维度 | TableGen 宿主语法（`TGParser` 真正执行的） | ODS 语义文法（本章） |
| --- | --- | --- |
| 谁执行 | `llvm/lib/TableGen/TGParser.cpp` | 没有 parser；由 ODS backend 在语义层"识别" |
| 终结符 | `def` `class` `let` `include` `IDENTIFIER` `STRING` `CODE` | `Op` `AttrDef` `assemblyFormat` `$operand` `attr-dict` 等语义关键字 |
| 在 TD 文件里的样子 | 就是文件本身 | 全部被压扁成 `IDENTIFIER` / `STRING_LITERAL` / `CODE_FRAGMENT` |
| 谁"识别"它 | `TGParser` | `getAllDerivedDefinitions("Op")`、`Operator(def)`、`OpFormatParser` |
| 能否表达"必须成对出现" | 不能（`def` 合法不等于 ODS 合法） | 能（产生式就是结构约束） |
| 用途 | 解析 `.td` | 形式化规范、合法性分析、设计类 ODS 语言时的参考 |

一句话：**`TGParser` 认的是宿主语言；`mlir-tblgen` 认的是本章这门语言。**
本目录 10–12 讲的是宿主层的 `include` 图，本章讲的是寄生层的结构文法。

分层关系：

```
        .mlir 文本
            ^  parse / print
            |  （见 01–09：mlir/lib/AsmParser）
     assemblyFormat 子语言          <-- ods-asm-format.y（§5）
            ^
            |  它是 Op/AttrDef 的一个"字段值"（TD 层是 STRING_LITERAL）
            |
        ODS 记录层                  <-- ods.y（§4）
            ^
            |  寄生
            |
     TableGen 宿主语法              <-- TGParser（本章不建模）
```

## 1. 三个事实决定了它必须是一份独立文法

### 1.1 实体的 kind 由基类链决定，不由关键字决定

TD 里没有"定义一个 Op"的语法。一个 record 之所以是 Op，是因为它
`isSubClassOf("Op")`：

```cpp
// mlir/tools/mlir-tblgen/OpGenHelpers.cpp:66-75
const Record *classDef = records.getClass("Op");
...
for (const Record *def : records.getDefs()) {
  if (!def->isSubClassOf(classDef))
    continue;
```

同样：

| 实体 | 选取方式 | 位置 |
| --- | --- | --- |
| `Dialect` | `getAllDerivedDefinitions("Dialect")` | `DialectGen.cpp:402` |
| `AttrDef` / `TypeDef` | `getAllDerivedDefinitionsIfDefined(...)` | `AttrOrTypeDefGen.cpp:938,944` |
| `Pass` | `getAllDerivedDefinitions("PassBase")` | `PassGen.cpp:36` |
| `EnumAttr` | `getAllDerivedDefinitionsIfDefined("EnumAttr")` | `EnumPythonBindingGen.cpp:144` |

所以本章文法里的 `KW_OP`、`KW_ATTR_DEF` 等**不是 TD 关键字**，而是
"这个 record 的基类链落在哪"的语义 token。

### 1.2 同一个 op 有三种等价写法

以 `arith.addi` 为例，以下三段在语义上等价：

```tablegen
// (a) MI 式：Arguments/Results 作为 mixin 出现在基类列表
def Arith_AddIOp : Arith_IntBinaryOpWithOverflowFlags<"addi", [Commutative]> {
  let summary = "integer addition operation";
}

// (b) body-let 式
def Arith_AddIOp : Arith_Op<"addi", [Commutative]> {
  let arguments = (ins Arith_SignlessIntegerOrIndexLike:$lhs, ...);
  let results   = (outs Arith_SignlessIntegerOrIndexLike:$result);
  let assemblyFormat = "...";
}

// (c) family 式：带模板参数的"op 规格"
class Arith_IntBinaryOpWithOverflowFlags<string mnemonic, list<Trait> traits = []> :
    Arith_BinaryOp<mnemonic, traits # [...]>,
    Arguments<(ins ...)>,
    Results<(outs ...)> {
  let assemblyFormat = "...";
}
```

（实际源码见 `ArithOps.td:193-215` 与 `ArithOps.td:271`。）

(a) 与 (b) 的差别只是 `Arguments<...>` / `Results<...>` 出现在**后缀**还是
**body**；(c) 多了模板参数，是一个"参数化的 op 规格"。文法用
`op_suffix_list` + `entity_body_opt` 统一 (a)(b)，用 `op_family_def` 表达 (c)。

### 1.3 字段是 `let`，而且有 `append` / `prepend`

ODS 的字段赋值在 TD 层是 `let <name> = <value>;`；TD 还允许合并模式：

```tablegen
let append items  = [4];   // 追加到当前值
let prepend items = [1];   // 前插
```

（`llvm/docs/TableGen/ProgRef.md:740-782`。）

这意味着"字段"不是简单的 `name = value`，而是带合并模式的赋值。文法用
`field_mode field_assign SEMI` 表达，`field_mode` 可取空 / `KW_APPEND` /
`KW_PREPEND`。

## 2. 交付物与验证

| 文件 | 内容 | 验证 |
| --- | --- | --- |
| `ods.y` | 记录层：实体、字段、值、约束表达式 | `bison -Wall -Wcounterexamples ods.y` → 0 冲突 |
| `ods-asm-format.y` | `assemblyFormat` 子语言 | `bison -Wall -Wcounterexamples` → 0 冲突 |

```
$ bison --version | head -1
bison (GNU Bison) 3.8.2
$ bison -Wall -Wcounterexamples -o /tmp/ods.c ana-docs/mlir/ods.y
$ bison -Wall -Wcounterexamples -o /tmp/odsaf.c ana-docs/mlir/ods-asm-format.y
$ echo $?
0
```

0 冲突不是巧合，而是设计约束：这份文法要被人类审阅，歧义会直接变成
"两种读法都成立"的规范漏洞。为此做了两处刻意的左因子化：

1. `Op<D, "x", [T]>, Arguments<...>, Results<...>` 里，traits 列表和
   `Arguments` / `Results` mixin 都以 `,` 开头，LR(1) 无法在只看 `,` 时决定
   走哪条路。文法把它们合并成 `op_suffix_list`，先吃掉 `,`，再看下一个 token。
2. `value` 与 `ctor_value` 都以 `IDENTIFIER` 开头。文法让
   `ctor_value : qualified_ident '<' ... '>'` 直接以 `qualified_ident` 为左部，
   把"是否跟 `<`"交给 LR(1) 的 lookahead 决定（`'<'` 不在 `FOLLOW(value)` 里）。

## 3. 词法层

### 3.1 基础 token

| token | 语义 | TD 层的来源 |
| --- | --- | --- |
| `IDENTIFIER` | 名字（类名、字段值、C++ 名） | `IDENTIFIER` |
| `STRING_LITERAL` | 字符串（summary、mnemonic、C++ 类型名） | `STRING` |
| `CODE_FRAGMENT` | `[{ ... }]` 代码块 | `CODE` |
| `INTEGER` | `0` / `1` / 枚举值 / 位宽 | `INT` |
| `VARIABLE` | `$lhs` 这类 dag 参数名 | `$` + `IDENTIFIER` |
| `L_*` / `R_*` / `COMMA` / `COLON` / `EQUAL` / `SEMI` / `QUESTION` | 结构符号 | 同名字符 |
| `SCOPE` | `::` | `::` |
| `DOT` | `.`（取字段，`X.predicate`） | `.` |
| `CONCAT` | `#`（值连接，`traits # [Pure]`） | `#` |
| `BANG` | `!`（bang 算子，`!if(...)`） | `!` |

### 3.2 实体与字段 token

| 语义 token | 覆盖的 TD 写法 | 定义位置 |
| --- | --- | --- |
| `KW_DIALECT` | `Dialect` | `DialectBase.td:22` |
| `KW_OP` | `Op<Dialect, mnemonic, props>` | `OpBase.td:345` |
| `KW_OP_FAMILY` | 带模板参数的 `class X<...> : Op<...>` | `ArithOps.td:193` |
| `KW_ATTR_DEF` / `KW_TYPE_DEF` | `AttrDef` / `TypeDef` | `AttrTypeBase.td:267,306` |
| `KW_*_INTERFACE` | `OpInterface` / `AttrInterface` / `TypeInterface` / `DialectInterface` | `Interfaces.td:132-166` |
| `KW_I32_ENUM_ATTR` / `KW_I64_ENUM_ATTR` / `KW_BIT_ENUM_ATTR` | `I32EnumAttr` / `I64EnumAttr` / `BitEnumAttr` | `EnumAttr.td:347,351,435` |
| `KW_PASS` | `Pass<"arg", "op">` | `PassBase.td:95` |
| `KW_PAT` | `Pat<pattern, result, ...>` | `PatternBase.td:120` |
| `KW_PROPERTY_DEF` | `Property<"T", "desc">` | `Properties.td:29` |
| `KW_TYPE_CONSTRAINT` / `KW_ATTR_CONSTRAINT` | `TypeConstraint` / `AttrConstraint` | `Constraints.td:178,189` |
| `KW_ARGUMENTS` / `KW_RESULTS` | `Arguments<...>` / `Results<...>` mixin | `OpBase.td:479,484` |
| `KW_INS` / `KW_OUTS` / `KW_REGION` / `KW_SUCCESSOR` | dag 的 operator marker | `Utils.td:57,60`、`OpBase.td:254,257` |

### 3.3 `CONSTRAINT_ATOM`：一个刻意开放的 token 类

约束原子（`I32`、`F32`、`Index`、`AnyInteger`、`AnyType`、`Attr`、`Value` …）
是开放集合：任何 `TypeConstraint` 子类都能出现在这个位置。文法不为它们逐一
开 token，而是用一个 `CONSTRAINT_ATOM` 类，并在词法上规定：

- 已知的原子约束名 → `CONSTRAINT_ATOM`
- 用户自定义的约束类名 → `IDENTIFIER`，走 `qualified_ident`

两条路径在文法里都归入 `type_constraint`，所以 `AnyTypeOf<[I32, MyConstraint]>`
合法。这个"开放 token 类"是本文法**故意保留的抽象**，理由见 §8 第 25 条。

## 4. 记录层文法（`ods.y`）

### 4.1 文件与实体

```yacc
ods_file   : %empty | ods_file ods_entity ;
ods_entity : dialect_def | op_def | op_family_def | attr_or_type_def
           | interface_def | enum_def | pass_def | pattern_def
           | property_def | constraint_def ;
```

`include` 不建模：它在 TD 层是词法替换（`ProgRef.md:242`，另见 11 章），
ODS backend 看到的是替换并求值后的 record 集合。`defvar` / `!foreach` /
`multiclass` 同理，属于宿主层的元编程。

所有实体共享一个可选的 body：

```yacc
entity_body_opt : %empty | '{' field_list '}' ;
```

### 4.2 Op 与 OpFamily

```yacc
op_def    : op_header op_suffix_list entity_body_opt opt_semi ;
op_header : KW_OP '<' dialect_ref ',' STRING_LITERAL '>' ;

op_suffix_list : %empty | op_suffix_list op_suffix ;
op_suffix
  : ',' list_value                                        /* [Trait1, Trait2] */
  | ',' KW_ARGUMENTS '<' '(' KW_INS  dag_arg_list_opt ')' '>'
  | ',' KW_RESULTS   '<' '(' KW_OUTS dag_arg_list_opt ')' '>'
  ;
```

三个设计点：

1. **第 3 个模板参数与 mixin 合并**。ODS 的位置参数是
   `Op<Dialect, mnemonic, props>`，`props` 是 traits 列表，写作 `, [T1, T2]`；
   MI 写法又在后面接 `, Arguments<...>, Results<...>`。两者都以 `,` 开头，
   合并后 LR(1) 只看 `,` 之后的 token 即可区分。
2. **`Arguments` / `Results` 只包装 dag，不引入新字段**：
   `class Arguments<dag args> { dag arguments = args; }`（`OpBase.td:479-486`），
   所以文法把它们建模成"给 `arguments` / `results` 字段赋值"的语法糖。
3. **`Regions` / `Successors` 没有 mixin 类**，它们只能是 body 字段
   （`let regions = (region ...)`）。文法不虚构 `Regions<...>`。

`OpFamily` 是语义 kind，TD 里写作带模板参数的 `class`：

```yacc
op_family_def
  : KW_OP_FAMILY '<' param_decl_list_opt '>'
    ':' family_base op_suffix_list entity_body_opt opt_semi ;
param_decl : qualified_ident opt_template_args IDENTIFIER opt_default ;
```

它把"op 规格"和"op 实例"分开：`Arith_IntBinaryOpWithOverflowFlags<...>` 是
family，`Arith_AddIOp` 是实例。这份区分在 TD 语法里靠"有没有模板参数"体现，
在语义文法里被显式命名。

### 4.3 AttrDef / TypeDef

```yacc
attr_or_type_def
  : attr_or_type_kind '<' dialect_ref ',' STRING_LITERAL
    attr_type_tail '>' entity_body_opt opt_semi ;

attr_type_tail
  : %empty
  | ',' list_value                        /* traits */
  | ',' list_value ',' STRING_LITERAL     /* traits, baseCppClass */
  ;
```

位置参数与 `AttrTypeBase.td:267,306` 一致：
`AttrDef<dialect, name, traits = [], baseCppClass = "::mlir::Attribute">`。
`attr_type_tail` 的两段可选，正是"默认参数不写"的语义。

`parameters` 也是 dag，operator 同样是 `ins`（`AttrTypeBase.td:203` 的
`dag parameters = (ins);`），所以与 `arguments` 同形：

```yacc
| F_PARAMETERS '=' '(' KW_INS dag_arg_list_opt ')'
```

### 4.4 Interface

```yacc
interface_def
  : interface_kind '<' STRING_LITERAL opt_base_interfaces '>'
    entity_body_opt opt_semi ;
interface_kind
  : KW_INTERFACE | KW_OP_INTERFACE | KW_ATTR_INTERFACE
  | KW_TYPE_INTERFACE | KW_DIALECT_INTERFACE ;
```

与 `Interfaces.td:99-110` 对应：`Interface<string name, list<Interface> baseInterfaces = []>`。
body 里的 `methods` / `cppIncludes` / `extraSharedClassDeclaration` 都是字段。

### 4.5 Enum：`cases` 是模板参数

这是上一版稿最容易写错的地方：

```yacc
enum_def : enum_kind '<' enum_params '>' entity_body_opt opt_semi ;
enum_params
  : STRING_LITERAL ',' STRING_LITERAL ',' enum_case_list
  | qualified_ident ',' STRING_LITERAL ',' STRING_LITERAL ',' enum_case_list
  ;
enum_case : enum_case_kind '<' STRING_LITERAL ',' INTEGER opt_string '>' ;
```

`I32EnumAttr<string name, string summary, list<I32EnumAttrCase> cases>`
（`EnumAttr.td:347`）——**`cases` 是第 3 个模板参数，不是 body 字段**。
`BitEnumAttr` 多一个前导 `I intType`（`EnumAttr.td:435`），所以
`enum_params` 有两条产生式；靠 `STRING_LITERAL` 与 `qualified_ident`
的首 token 区分，无歧义。

### 4.6 Pass / Pattern / Property / Constraint

```yacc
pass_def     : KW_PASS '<' STRING_LITERAL opt_pass_operation '>' body? ;
pattern_def  : KW_PAT '<' value_list_opt '>' ;
property_def : KW_PROPERTY_DEF '<' value_list_opt '>' ;
constraint_def : constraint_kind '<' value_list '>' body? ;
```

`Pass<string passArg, string operation = "">`（`PassBase.td:95`）、
`Pat<dag, dag, list<dag> preds = [], ...>`（`PatternBase.td:120`）、
`Property<string storageType = "", ...>`（`Properties.td:29`）都是"位置参数
带默认值"，所以 `pass_def` / `pattern_def` / `property_def` 允许可变元数，
元数是否合法留给语义检查（§8 第 28 条）。

### 4.7 body 字段

```yacc
field      : field_mode field_assign SEMI ;
field_mode : %empty | KW_APPEND | KW_PREPEND ;

field_assign
  /* 结构上有独立语义的 dag 字段 */
  : F_ARGUMENTS  '=' '(' KW_INS  dag_arg_list_opt ')'
  | F_RESULTS    '=' '(' KW_OUTS dag_arg_list_opt ')'
  | F_REGIONS    '=' '(' KW_REGION dag_arg_list_opt ')'
  | F_SUCCESSORS '=' '(' KW_SUCCESSOR dag_arg_list_opt ')'
  | F_PARAMETERS '=' '(' KW_INS dag_arg_list_opt ')'
  /* 列表字段 */
  | F_TRAITS '=' list_value
  | F_BASE_INTERFACES '=' list_value
  | F_BUILDERS '=' list_value
  | F_OPTIONS '=' list_value
  | F_METHODS '=' list_value
  | F_CPP_INCLUDES '=' list_value
  /* 字符串 / 代码字段 */
  | F_ASSEMBLY_FORMAT '=' text_value
  | F_SUMMARY     '=' text_value
  | F_DESCRIPTION '=' text_value
  | F_CPP_NAMESPACE '=' STRING_LITERAL
  | F_MNEMONIC    '=' STRING_LITERAL
  | F_EXTRA_CLASS_DECL '=' CODE_FRAGMENT
  ...
  /* 其余字段：语义同构 */
  | IDENTIFIER '=' value
  ;
```

`text_value : STRING_LITERAL | CODE_FRAGMENT` 是必需的：`description` 与
`assemblyFormat` 在真实代码里绝大多数写成 `[{ ... }]`（本仓库 `mlir/include` 下
`let description = [{` 出现 2766 次、`let assemblyFormat = [{` 出现 844 次），
这是 TD 的 code literal，不是 `"..."`。

这里做了取舍：只把**结构上不一样**的字段单列（dag、列表、字符串、代码），
`bit` 标志（`hasVerifier`、`hasFolder`、`skipDefaultBuilders` …）走
`IDENTIFIER '=' value`，因为它们在语义上就是"名字 = 值"，单列只会把词法表
膨胀几十个 token 而不增加信息。

字段清单本身是文法的一部分，本章不逐一重复；`Op` 的 25 个字段见
`OpBase.td:345-478`，`AttrOrTypeDef` 的 19 个字段见
`AttrTypeBase.td:161-265`，`Interface` 的 9 个字段见 `Interfaces.td:99-138`。

### 4.8 值层

```yacc
value         : concat_expr ;
concat_expr   : postfix_value | concat_expr CONCAT postfix_value ;   /* # */
postfix_value : primary_value | postfix_value DOT IDENTIFIER ;       /* .field */

primary_value
  : STRING_LITERAL | INTEGER | CODE_FRAGMENT | QUESTION
  | type_constraint | dag_value | list_value
  | ctor_value | builder_value | bang_value
  ;

bang_value  : BANG IDENTIFIER '(' value_list_opt ')' ;               /* !if(...) */
dag_value   : '(' dag_op dag_arg_list_opt ')' ;
dag_op      : KW_INS | KW_OUTS | KW_REGION | KW_SUCCESSOR | qualified_ident ;
dag_arg     : value opt_arg_name ;
opt_arg_name: %empty | ':' VARIABLE ;

list_value  : '[' value_list_opt ']' ;
ctor_value  : qualified_ident '<' value_list_opt '>' ;
```

三层是必要的，因为 TD 的 `Init` 表达式确实有这三个优先级层次：

| 层 | TD 写法 | 典型 ODS 场景 |
| --- | --- | --- |
| `#`（连接） | `traits # [Pure]` | 基类把 traits 拼接后传给 `Op<...>`（`ArithOps.td:194`） |
| `.`（取字段） | `X.predicate` | 将原子约束的谓词组合进 `Or<[...]>`（`ArithOps.td:159-164`） |
| `!op(...)` | `!if(cond, a, b)` | 条件化字段值 |

优先级：`.` 高于 `#`，所以 `A.predicate # [B]` 先取字段再连接。

`dag_value` 同时覆盖两件事：

- `statistics` / `benefit` 这类普通 dag：operator 是 `qualified_ident`
  （如 `(addBenefit 0)`）；
- `arguments` / `results` / `regions` / `successors` 的 dag：operator 是
  `ins` / `outs` / `region` / `successor`（`Utils.td:57,60`）。

`builder_value` 是唯一有独立参数形状的构造器：

```yacc
builder_value
  : KW_OP_BUILDER '<' '(' builder_ins_list_opt ')' builder_body_opt '>' ;
builder_ins  : STRING_LITERAL ':' VARIABLE ;          /* "Value":$lhs */
builder_body_opt : %empty | ',' CODE_FRAGMENT | ',' STRING_LITERAL ;
```

`OpBuilder<(ins "Value":$lhs, "Value":$rhs), [{ ... }]>` 的 `ins` 关键字在这里
被省略了——它是 `OpBuilder` 的参数列表助记符，不携带额外语义；若想严格对应
TD 写法，可把 `'('` 改为 `'(' KW_INS`。

### 4.9 约束表达式

```yacc
type_constraint
  : CONSTRAINT_ATOM
  | qualified_ident                       /* 用户自定义约束类 */
  | pred_expr
  | KW_ANY_TYPE_OF '<' list_value '>'
  | KW_ALL_OF_TYPE '<' list_value '>'
  | KW_OPTIONAL    '<' type_constraint '>'
  | KW_VARIADIC    '<' type_constraint '>'
  | KW_VARIADIC_OF_VARIADIC '<' type_constraint ',' STRING_LITERAL '>'
  | KW_PROPERTY    '<' value_list_opt '>'
  ;

pred_expr
  : KW_CPRED '<' STRING_LITERAL '>'
  | KW_CPRED '<' STRING_LITERAL ',' STRING_LITERAL '>'
  | KW_AND '<' list_value '>'
  | KW_OR  '<' list_value '>'
  | KW_NEG '<' pred_expr '>'
  ;
```

对应源码：

| 构造 | 签名 | 位置 |
| --- | --- | --- |
| `Optional<T>` | `class Optional<Type type>` | `CommonTypeConstraints.td:152` |
| `Variadic<T>` | `class Variadic<Type type>` | `CommonTypeConstraints.td:133` |
| `VariadicOfVariadic<T,"x">` | `class VariadicOfVariadic<Type type, string name>` | `CommonTypeConstraints.td:145` |
| `AnyTypeOf<[...]>` | `class AnyTypeOf<list<Type>, ...>` | `CommonTypeConstraints.td:198` |
| `AllOfType<[...]>` | `class AllOfType<list<Type>, ...>` | `CommonTypeConstraints.td:211` |
| `And<[...]>` / `Or<[...]>` / `Neg<P>` | `list<Pred>` / `Pred` | `Constraints.td:87,91,94` |
| `CPred<"...">` | `class CPred<code pred> : Pred` | `Constraints.td:63` |

**注意不存在 `AnyOf` / `AllOf`。** 见 §7 勘误 2。

## 5. `assemblyFormat` 子语言（`ods-asm-format.y`）

### 5.1 为什么它是独立子语言

在 TD 层，`assemblyFormat` 只是一个字符串：

```tablegen
let assemblyFormat = "$lhs `,` $rhs (`overflow` `` $overflowFlags^)?
                      attr-dict `:` type($result)";
```

`OpFormatGen.cpp` 会把这个字符串**再解析一次**，得到 `FormatElement` 树
（`FormatGen.h:169`：`Literal` / `String` / `Variable` / `Whitespace` /
`Directive` / `Optional`）。所以它是"字段值里的另一门语言"——本章把它单列。

### 5.2 token 表

来自 `FormatGen.h:40-82`：

| 类别 | token |
| --- | --- |
| 结构 | `(` `)` `^` `:` `,` `=` `<` `>` `?` `*` `\|` |
| 关键字 | `attr-dict` `attr-dict-with-keyword` `prop-dict` `custom` `enum` `functional-type` `oilist` `operands` `params` `qualified` `ref` `regions` `results` `struct` `successors` `type` |
| 带值 | `identifier` `literal`（`` `...` ``） `variable`（`$x`） `string`（`"..."`） |

`literal` 里的 `` `` `` 和 `` `\n` `` 会变成 `WhitespaceElement`，其余变成
`LiteralElement`；合法标点表见 `FormatGen.cpp:514-542` 与
`mlir/docs/DefiningDialects/Operations.md:735-1090`。

### 5.3 文法主干

```yacc
format          : top_element_list_opt ;
top_element
  : literal | variable
  | attr_dict | prop_dict | operands | regions | results | successors
  | type_directive | functional_type | enum_directive | qualified_directive
  | custom_directive | oilist_directive | params_directive | struct_directive
  | optional_group
  ;
```

每个指令的参数形状：

```yacc
type_directive      : KW_TYPE '(' type_operand ')' ;
type_operand        : variable | operands | results ;
functional_type     : KW_FUNCTIONAL_TYPE '(' type_operand ',' type_operand ')' ;
enum_directive      : KW_ENUM '(' variable ')' ;
qualified_directive : KW_QUALIFIED '(' (variable | type_directive) ')' ;
```

注意 `type(operands)` / `type(results)` 是**整组绑定**，不是
`type($operands)`——`parseTypeDirectiveOperand` 接受的是
`OperandsDirective` / `ResultsDirective`（`OpFormatGen.cpp:4607-4653`）。

### 5.4 上下文路由：同一 token 在不同位置合法性不同

这是 `assemblyFormat` 最容易被忽略的部分：`parseElement(Context)` 用一个
`Context` 枚举控制"此处允许哪种元素"（`FormatGen.cpp:229-330`）。

| 元素 | 允许的上下文 | 违反时 | 位置 |
| --- | --- | --- | --- |
| literal（反引号） | 仅 top-level | "literals may only be used in the top-level section" | `FormatGen.cpp:252` |
| string（双引号） | 仅 `custom(...)` 参数 | "strings may only be used as 'custom' directive arguments" | `FormatGen.cpp:282` |
| optional group | 仅 top-level | "optional groups can only be used as top-level elements" | `FormatGen.cpp:326` |
| `custom<...>` | top-level 或 `struct(...)` 内 | "`custom` can only be used at the top-level context or within a `struct`" | `FormatGen.cpp:405` |
| `ref(...)` | 仅 `custom(...)` 内 | "'ref' is only valid within a `custom` directive" | `FormatGen.cpp:449` |
| `params` | top-level 或 `struct(...)` 内 | — | `AttrOrTypeFormatGen.cpp:1344` |
| `struct(...)` | 仅 top-level | — | `AttrOrTypeFormatGen.cpp:1369` |
| `oilist` | top-level（其分隔符是 literal，受 literal 规则约束） | — | `OpFormatGen.cpp:4467` |

文法用不同的 nonterminal 把这个上下文编码进去：`top_element` 含
`optional_group` / `struct_directive`；`custom_arg` 只含
`STRING_LITERAL` / `variable` / `attr_dict` / `prop_dict` / `ref_directive` /
`type_directive` / `qualified_directive`（`verifyCustomDirectiveArguments`
的白名单，`OpFormatGen.cpp:4366-4382`）；`ref_arg` 是另一组。

### 5.5 optional group：`^` 锚点是必需的

官方文档给的形状（`Operations.md:1019-1060`）：

```
optional-group: `(` then-elements `)` (`:` `(` else-elements `)`)? `?`
```

锚点不在这个形状里，因为它是**元素的后缀**：某个元素后面跟 `^`。实现
（`FormatGen.cpp:321-400`）的真实规则是：

1. 解析 `then` 元素，每解析完一个就看下一个 token 是否是 `^`；是则把刚解析的
   元素记为 anchor。
2. 若有 `: ( ... )`，同样解析 `else`，anchor 仍可能在这里被设置。
3. 两种分支都解析完后，**必须有且只有一个 anchor**（"only one element can be
   marked as the anchor"，`FormatGen.cpp:342`；"optional group has no anchor
   element"，`:376`）。
4. `then` 的第一个可解析元素必须是 literal / variable / custom directive
   （`FormatGen.cpp:394`）。

对应的产生式：

```yacc
optional_group : '(' group_then opt_else_group ')' '?' ;
opt_else_group : %empty | ':' '(' group_else ')' ;
group_element  : top_element | top_element CARET ;
```

`arith.addi` 的写法正好是一个带 `else` 的例子：

```tablegen
$lhs `,` $rhs (`overflow` `` $overflowFlags^)? attr-dict `:` type($result)
```

` `` `（空 literal）会变成 `WhitespaceElement`，用于在打印时抑制自动空格。

### 5.6 oilist

```yacc
oilist_directive   : KW_OILIST oilist_separator_opt '(' oilist_clause_list ')' ;
oilist_separator_opt : %empty | '<' literal '>' ;
oilist_clause_list : oilist_clause | oilist_clause_list '|' oilist_clause ;
oilist_clause      : literal top_element_list_opt ;
```

来自 `OpFormatGen.cpp:4467-4513`。分隔符必须是**非空白 literal**
（`:4478`）；每个 clause 以一个字面关键字开头，后跟该 clause 的元素；clause
之间用 `|` 分隔。

## 6. 完整示例：`arith.addi` 在两种读法下的推导

源码（`ArithOps.td:193-215, 271`，已省略号截断）：

```tablegen
class Arith_IntBinaryOpWithOverflowFlags<string mnemonic, list<Trait> traits = []> :
    Arith_BinaryOp<mnemonic, traits # [Pure, ...]>,
    Arguments<(ins Arith_SignlessIntegerOrIndexLike:$lhs,
                   Arith_SignlessIntegerOrIndexLike:$rhs,
                   DefaultValuedAttr<...>:$overflowFlags)>,
    Results<(outs Arith_SignlessIntegerOrIndexLike:$result)> {
  let assemblyFormat = [{ $lhs `,` $rhs (`overflow` `` $overflowFlags^)?
                            attr-dict `:` type($result) }];
}

def Arith_AddIOp : Arith_IntBinaryOpWithOverflowFlags<"addi", [Commutative, ...]> {
  let summary = "integer addition operation";
  let description = [{ ... }];
}
```

**TD parser 看到的**：一个 `class`（带 2 个模板参数）、一个 `def`（带 2 个模板
实参）、若干 `let`。它不知道 `Arith_BinaryOp` 是 op 基类，也不知道
`Arguments<...>` 会给 `arguments` 字段赋值。

**ODS backend 看到的**（本章文法）：

| TD 文本 | 语义文法产生式 |
| --- | --- |
| `def Arith_AddIOp` | `op_def` |
| `: Arith_IntBinaryOpWithOverflowFlags<"addi", [...]>` | `op_header` + `op_suffix`（第 3 参数是 traits 列表） |
| `Arguments<(ins ...:$lhs, ...:$rhs, ...:$overflowFlags)>` | `op_suffix : ',' KW_ARGUMENTS '<' '(' KW_INS dag_arg_list_opt ')' '>'` |
| `Results<(outs ...:$result)>` | `op_suffix : ',' KW_RESULTS '<' '(' KW_OUTS dag_arg_list_opt ')' '>'` |
| `let summary = "..."` | `field : field_mode field_assign SEMI`，`F_SUMMARY '=' STRING_LITERAL` |
| `let assemblyFormat = [{...}]` | `F_ASSEMBLY_FORMAT '=' text_value`，内容交给 §5 的文法 |

`Arith_IntBinaryOpWithOverflowFlags` 本身则是一条 `op_family_def`：

```
KW_OP_FAMILY '<' param_decl_list '>' ':' family_base op_suffix_list '{' ... '}'
```

如果把它内联展开成 `arith.addi` 的 body-let 写法，就是 §1.2 的 (b)：
`Arguments<...>` / `Results<...>` 从 `op_suffix` 变成
`F_ARGUMENTS` / `F_RESULTS` 字段，语义不变。

## 7. 勘误：上一版 AI 稿的 5 处事实错误

| # | 上一版写法 | 事实 | 源码 |
| --- | --- | --- | --- |
| 1 | 给 `Op` 加了 `interfaces` / `verifier` 字段 | `Op` **没有**这两个字段。op interface 一律走 trait（`DeclareOpInterfaceMethods<...>`）；自定义验证用 `let hasVerifier = 1` + C++ `verify()` | `OpBase.td:345-478`（字段表见 `:439,448`） |
| 2 | 用 `AnyOf` / `AllOf` 作组合子 | **不存在**。列表型是 `AnyTypeOf<[...]>` / `AllOfType<[...]>`；谓词型是 `And<[Pred]>` / `Or<[Pred]>` / `Neg<Pred>` | `CommonTypeConstraints.td:198,211`；`Constraints.td:87,91,94` |
| 3 | `I32EnumAttr { let cases = [...] }` | `cases` 是**第 3 个模板参数**，不是 body 字段 | `EnumAttr.td:347` |
| 4 | `type($var)` / `functional-type($a, $b)` 只取单变量 | 还接受整组指令：`type(operands)`、`type(results)`；`functional-type(operands, results)` 同理 | `OpFormatGen.cpp:4607-4653` |
| 5 | optional group 写作 `( ... )?` | 真实形状是 `( then [^] [: ( else )] )?`，且**必须有且只有一个 `^` 锚点** | `FormatGen.cpp:321-400` |

第 1 条值得展开：`Op` 的完整字段集合是
`opDialect, opName, cppNamespace, summary, description, opDocGroup, partition,
arguments, results, regions, successors, builders, skipDefaultBuilders,
assemblyFormat, hasCustomAssemblyFormat, hasCustomPropertiesPrinter,
hasVerifier, hasRegionVerifier, hasCanonicalizer, hasCanonicalizeMethod,
hasFolder, useCustomPropertiesEncoding, traits, extraClassDeclaration,
extraClassDefinition`。接口能力通过 `traits` 里的 `DeclareOpInterfaceMethods<...>`
注入，而不是字段。

除这 5 处事实错误，上一版还有 3 处**覆盖缺口**（不是写错，是没覆盖到真实写法）：

1. `parameters` 的 dag operator 也是 `ins`（`AttrTypeBase.td:203`）；
2. `summary` / `description` / `assemblyFormat` 大量写成 `[{ ... }]` code literal，
   不能只建模 `"..."`（`description` 2766 次、`assemblyFormat` 844 次）；
3. `#` 值连接（`traits # [Pure]`）、`.field` 取字段（`X.predicate`）、
   `!op(...)` bang 表达式（`!if`）都是 ODS 定义里常见的值语法。

## 8. 文法表达不了的语义约束

这份文法的价值有一半在"它故意没说什么"。以下约束**语法层无法表达**，由
backend 用 C++ 检查；把它们列出来，才能区分"结构合法"与"ODS 合法"。

| # | 约束 | 检查点 |
| --- | --- | --- |
| 1 | `results` 的 dag operator 必须是 `outs` | `Operator.cpp:717-719` |
| 2 | `successors` 的 dag operator 必须是 `successor` | `Operator.cpp:748-751` |
| 3 | `arguments` 元素只能是 `TypeConstraint` / `Attr` / `Property` | `Operator.cpp:646-666` |
| 4 | 出现在 argument list 里的 attr / property 必须具名 | `Operator.cpp:651,659` |
| 5 | derived attr 不能进 argument list | `Operator.cpp:653-655` |
| 6 | `VariadicOfVariadic` 只能用于 operand | `Operator.cpp:737-742` |
| 7 | 只有最后一个 successor 可以是 variadic | `Operator.cpp:762-764` |
| 8 | `assemblyFormat` 必须包含 `attr-dict` | `OpFormatGen.cpp:3573-3576` |
| 9 | `$x` 必须引用已声明的 operand/result/attr/region/successor/property | `OpFormatGen.cpp` 的 `parseVariableImpl` |
| 10 | `attr-dict` 只能出现一次 | `OpFormatGen.cpp:4342` |
| 11 | `prop-dict` 只能出现一次 | `OpFormatGen.cpp:4360` |
| 12 | `enum` 与 `qualified` 互斥 | `OpFormatGen.cpp:4320-4322` |
| 13 | `enum` 要求 enum attr；bit enum 必须有 0 值 case | `OpFormatGen.cpp:4296-4318` |
| 14 | optional group 必须有且只有一个 anchor | `FormatGen.cpp:342,376` |
| 15 | optional group 的 `then` 首个可解析元素必须是 literal/variable/custom | `FormatGen.cpp:394` |
| 16 | literal 只能出现在 top-level | `FormatGen.cpp:252` |
| 17 | string 只能作为 `custom` 参数 | `FormatGen.cpp:282` |
| 18 | `custom` 参数只能是指定白名单元素 | `OpFormatGen.cpp:4366-4382` |
| 19 | `oilist` 分隔符必须是非空白 literal | `OpFormatGen.cpp:4478` |
| 20 | `oilist` 组内只能出现 optional/default attr、optional property 等 | `OpFormatGen.cpp:4528-4545` |
| 21 | AttrOrType 格式必须引用所有非 optional 参数 | `AttrOrTypeFormatGen.cpp:1163-1179` |
| 22 | 带 optional 参数的 `struct` 后不能紧跟逗号 literal | `AttrOrTypeFormatGen.cpp:1180-1188` |
| 23 | `Partition` 名字必须是 C++ 标识符 | `DialectBase.td:146-147`（`assert !match(...)`) |
| 24 | `OpBuilder` 的 `"Value"` / `"int"` 等必须是合法 C++ 类型名 | 生成 C++ 签名时 |
| 25 | `And` / `Or` / `Neg` 的子树必须是 `Pred`；`AnyTypeOf` / `AllOfType` 的子树必须是 `Type` | 各自的类签名（§4.9） |
| 26 | `type($x)` 只能绑定一次；`ref(type($x))` 只能引用已绑定的 `type` | `OpFormatGen.cpp:4607-4653` |
| 27 | `FunctionalType` 的两个参数只能是 variable / `operands` / `results` | `OpFormatGen.cpp:4607-4653` |
| 28 | `Pat` / `Pass` / `Property` 的位置参数元数 | 各自的 class 签名 |
| 29 | `Op` 的 kind 判断（是否 `isSubClassOf("Op")`） | `OpGenHelpers.cpp:66-75` |

第 25 条是本章文法最容易引起误解的地方：`And<[I32]>` 在**语法**上完全符合
`pred_expr`，但 `And` 要的是 `list<Pred>`，`I32` 是 `Type`，所以它在
TableGen 的类型检查阶段就会被拒绝。这类"上下文无关文法接受、类型系统拒绝"
的组合，正是"语义文法"与"可执行文法"的分界线。

## 9. 与 backend 的对应关系

| 本章区域 | 消费代码 |
| --- | --- |
| 实体发现（§4.1） | `OpGenHelpers.cpp:63`、`DialectGen.cpp:402`、`AttrOrTypeDefGen.cpp:938,944`、`PassGen.cpp:36` |
| Op 字段（§4.2, §4.7） | `mlir/lib/TableGen/Operator.cpp:41-`（构造时逐字段求值） |
| 约束表达式（§4.9） | `mlir/lib/TableGen/Constraint.cpp`、`Predicate.cpp` |
| `assemblyFormat` 词法与结构（§5.3） | `mlir/tools/mlir-tblgen/FormatGen.cpp` |
| `assemblyFormat` 的 Op 语义（§5.4-5.6） | `mlir/tools/mlir-tblgen/OpFormatGen.cpp` |
| AttrOrType 的 `params` / `struct`（§5.4） | `mlir/tools/mlir-tblgen/AttrOrTypeFormatGen.cpp` |
| 生成的 `.inc` 进入 C++ | `OpDefinitionsGen.cpp`（`-gen-op-decls/-defs`） |

## 10. 结论

1. **ODS 有两层，本章写的是第二层。** 第一层是 TD 宿主语法（`TGParser` 执行），
   第二层是 ODS 语义结构（backend 识别）。把第二层写成 yacc 是**设计**，
   不是抄录——没有源码可以逐行对照，因为实现是 C++ 的 `Record` 遍历。
2. **一句话能说清这份文法的定位**：它的每个产生式，都对应 TD 里一段被压扁的
   `def` / `class` / `let`；它描述的是 backend "期望看到"的结构。
3. **0 冲突是刻意维持的**：歧义在规范里就是漏洞。两处左因子化（op 后缀列表、
   `value`/`ctor_value`）值得在审阅时重点看。
4. **文法的边界比文法本身更重要**：§8 的 29 条约束说明，"结构合法"离
   "ODS 合法"还有很远；这也解释了为什么 `mlir-tblgen` 需要几千行 C++ 做语义检查，
   而不是一个 yacc 解析器。
