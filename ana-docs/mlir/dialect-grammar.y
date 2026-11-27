/*===-- dialect-grammar.y - grammar induced from the corpus -----*- yacc -*-===//
 *
 * 这门文法不是先设计的，而是从 corpus/*.dialect 反推出来的：
 *   - 每条产生式至少有一个语料见证；
 *   - 语料没用到的构造不写进来（partial custom、oilist、interface 定义等
 *     留给下一轮语料）。
 *
 * 约定：
 *   - 小写关键字 = 声明与类型世界（dialect/op/schema/constraint/...）；
 *   - 首字母大写关键字 = 定义子句（Doc/Operands/Results/...）；
 *   - 子句以 `;` 结尾，以 `}` 结尾的块状子句（Assembly { ... }）除外；
 *   - 名字允许 `.`（词法器折进同一个 IDENT），`reduce.return` 无需转义；
 *   - 声明关键字是软关键字：`name` 也接受它们，因为 gpu.all_reduce 的
 *     属性就叫 `op`；
 *   - `type`/`if`/`attr-dict`/`attr-dict-with-keyword`/`functional-type`/
 *     `oilist`/`custom` 只在 Assembly 模式里是关键字，所以 `op if`、
 *     `type_def type` 在普通模式里都合法；
 *   - 谓词：列表是合取，`|` 是析取，没有 `and`/`not`。
 *
 * 验证：bison -Wall -Wcounterexamples dialect-grammar.y
 *===----------------------------------------------------------------------===*/

%start file

%token IDENT STRING INT
%token L_BRACE R_BRACE L_PAREN R_PAREN L_BRACKET R_BRACKET L_ANGLE R_ANGLE
%token COMMA SEMI COLON EQUAL PIPE DOT

/* 声明：小写 */
%token KW_DIALECT KW_OP KW_SCHEMA KW_CONSTRAINT KW_ENUM KW_ATTR_DEF KW_TYPE_DEF
%token KW_FROM KW_EXTENDS KW_BITS KW_OPTIONAL KW_VARIADIC

/* 子句：首字母大写 */
%token KW_DOC KW_OPERANDS KW_RESULTS KW_ATTRIBUTES KW_REGIONS KW_SUCCESSORS
%token KW_PROPERTIES KW_IMPLEMENTS KW_ASSEMBLY KW_CUSTOM
%token KW_CASES KW_GROUPS KW_PARAMETERS
%token KW_GENERIC

/* Assembly 模式 */
%token A_ATTR_DICT A_ATTR_DICT_WITH_KEYWORD A_TYPE A_IF
%token A_FUNCTIONAL_TYPE A_QUALIFIED A_OILIST A_CUSTOM A_SUCCESSORS

%define parse.trace

%code provides {
int yylex(void);
void yyerror(const char *);
}

%%

/*===--------------------------------------------------------------------===*/
/* 文件：一个方言                                                        */
/*===--------------------------------------------------------------------===*/

file
  : dialect_decl
  ;

dialect_decl
  : KW_DIALECT IDENT L_BRACE dialect_item_list R_BRACE
  ;

dialect_item_list
  : dialect_item
  | dialect_item_list dialect_item
  ;

dialect_item
  : doc_clause
  | constraint_decl
  | enum_decl
  | schema_decl
  | op_decl
  | attr_type_decl
  ;

/*===--------------------------------------------------------------------===*/
/* 名字与类型                                                            */
/*===--------------------------------------------------------------------===*/

/* 名字里的 `.` 是词法事实（词法器把它折进同一 token），语法层就是 IDENT。
 *
 * 声明关键字是"软关键字"：只在声明/子句的起点有意义，出现在名字位置时
 * 仍是名字。gpu.all_reduce 的属性就叫 `op`，不能因此改语言。 */
name
  : IDENT
  | decl_keyword
  ;

decl_keyword
  : KW_DIALECT | KW_OP | KW_SCHEMA | KW_CONSTRAINT | KW_ENUM
  | KW_ATTR_DEF | KW_TYPE_DEF | KW_FROM | KW_EXTENDS | KW_BITS
  | KW_OPTIONAL | KW_VARIADIC
  ;

constraint_decl
  : KW_CONSTRAINT name EQUAL type_expr SEMI
  ;

type_expr
  : type_union
  ;

type_union
  : type_primary
  | type_union PIPE type_primary
  ;

type_primary
  : IDENT type_postfix_opt
  | KW_OPTIONAL L_ANGLE type_expr R_ANGLE
  | KW_VARIADIC L_ANGLE type_expr R_ANGLE
  ;

type_postfix_opt
  : %empty
  | L_ANGLE type_arg_list R_ANGLE      /* Cast<index, fixed_int_like> */
  | L_PAREN IDENT R_PAREN              /* type_of(lhs) */
  ;

/* 类型实例的实参可以是类型，也可以是整数：`Cast<index, fixed_int_like>`
 * 传类型约束，`tosa.shape<4>` 传 rank——rank 是值，不是类型。 */
type_arg_list
  : type_arg
  | type_arg_list COMMA type_arg
  ;

type_arg
  : type_expr
  | INT
  ;

/* 引用一个 schema 或接口：`IntBinaryWithOverflow`、`Cast<index, int_like>` */
type_ref
  : name type_ref_args_opt
  ;

type_ref_args_opt
  : %empty
  | L_ANGLE type_arg_list R_ANGLE
  ;

type_ref_list
  : type_ref
  | type_ref_list COMMA type_ref
  ;

/*===--------------------------------------------------------------------===*/
/* schema / op                                                           */
/*===--------------------------------------------------------------------===*/

schema_decl
  : KW_SCHEMA name type_params_opt extends_opt
    L_BRACE clause_list_opt R_BRACE
  ;

type_params_opt
  : %empty
  | L_ANGLE param_list R_ANGLE
  ;

param_list
  : param
  | param_list COMMA param
  ;

param
  : name COLON type_expr
  ;

extends_opt
  : %empty
  | KW_EXTENDS type_ref
  ;

op_decl
  : KW_OP name from_opt L_BRACE clause_list_opt R_BRACE
  ;

from_opt
  : %empty
  | KW_FROM type_ref
  ;

clause_list_opt
  : %empty
  | clause_list
  ;

clause_list
  : clause
  | clause_list clause
  ;

clause
  : doc_clause
  | KW_OPERANDS L_PAREN binding_list R_PAREN SEMI
  | KW_RESULTS L_PAREN binding_list R_PAREN SEMI
  | KW_ATTRIBUTES L_PAREN binding_list R_PAREN SEMI
  | KW_REGIONS L_PAREN region_binding_list R_PAREN SEMI
  | KW_SUCCESSORS L_PAREN binding_list R_PAREN SEMI
  | KW_PROPERTIES L_BRACKET fact_list R_BRACKET SEMI
  | KW_IMPLEMENTS L_BRACKET type_ref_list R_BRACKET SEMI
  | KW_ASSEMBLY assembly
  ;

binding_list
  : binding
  | binding_list COMMA binding
  ;

/* 部件的值域事实挂在部件上，与 region 的注解同一个机制：
 * `Attributes (count: i64 [positive])`、`Operands (operand: any [non_negative])`。
 * 它是"这个属性的取值受限"，不是类型的一部分——同是 i64，一个受限一个不受。 */
binding
  : name COLON type_expr binding_annots_opt default_opt
  ;

binding_annots_opt
  : %empty
  | L_BRACKET fact_list R_BRACKET
  ;

/* region 的块数约束是 region 自己的事实，不是 op 的：
 * `Regions (body: region [single_block])`。
 * 一个变长 region 组也可以带约束，表示"每个"：
 * `Regions (case_regions: variadic<region> [single_block])`。 */
region_binding_list
  : region_binding
  | region_binding_list COMMA region_binding
  ;

region_binding
  : name COLON type_expr region_annots_opt
  ;

region_annots_opt
  : %empty
  | L_BRACKET fact_list R_BRACKET
  ;

default_opt
  : %empty
  | EQUAL expr
  ;

/* 默认值：`none` 这类标识符，或 `1` 这类整数（gpu.subgroup_reduce
 * 的 cluster_stride）。字符串默认值仍无见证。 */
expr
  : IDENT
  | INT
  ;

/* 语义事实是一个布尔表达式：
 *   - 列表 `[f1, f2]` 是合取（无逗号即无 "and" 关键字）；
 *   - `|` 是析取，优先级低于列表项，可组合任意多个原子；
 *   - 没有 `not`：否定一个结构性事实通常意味着该换个谓词。
 * 原子可以带实参，实参可以是名字或整数：has_parent(func)、inputs(2)。 */
fact_list
  : fact
  | fact_list COMMA fact
  ;

fact
  : fact_atom
  | fact PIPE fact_atom
  ;

fact_atom
  : IDENT
  | IDENT L_PAREN fact_arg_list R_PAREN
  ;

fact_arg_list
  : fact_arg
  | fact_arg_list COMMA fact_arg
  ;

fact_arg
  : name
  | INT
  ;

/*===--------------------------------------------------------------------===*/
/* Assembly：块状双向程序，或整体交给钩子                                */
/*===--------------------------------------------------------------------===*/

/* 文本通道必须显式写出：块格式、整体交给钩子、或明确"没有自定义格式"。
 * 省略 Assembly 子句不被解释为 generic——那样逃逸点就成了看不见的黑洞。 */
assembly
  : L_BRACE asm_list R_BRACE
  | KW_CUSTOM SEMI
  | KW_GENERIC SEMI
  ;

asm_list
  : asm_element
  | asm_list asm_element
  ;

asm_element
  : STRING                       /* 字面量，含标点 */
  | IDENT                        /* 引用一个已声明的部件 */
  | A_ATTR_DICT                  /* 剩余属性集合，输出为 {..} */
  | A_ATTR_DICT_WITH_KEYWORD     /* 剩余属性集合，输出为 attributes {..} */
  | A_TYPE L_PAREN IDENT R_PAREN /* 打印某个部件的类型 */
  | A_QUALIFIED L_PAREN A_TYPE L_PAREN IDENT R_PAREN R_PAREN
                                 /* qualified(type(x))：强制带方言前缀打印 */
  | A_FUNCTIONAL_TYPE L_PAREN IDENT COMMA IDENT R_PAREN
                                 /* (T...) -> (U...)，实参是部件组 */
  | A_CUSTOM L_ANGLE IDENT R_ANGLE L_PAREN custom_arg_list R_PAREN
                                 /* 局部子格式交给具名钩子 */
  | A_OILIST L_PAREN oilist_alt_list R_PAREN
                                 /* 至多出现一个可选组 */
  | A_IF IDENT L_BRACE asm_list R_BRACE
  | A_SUCCESSORS                  /* 打印全部后继，名字不再逐个写出 */
  ;

/* custom<Hook>(a, b)：钩子名是格式的实现细节，实参是部件引用。
 * `type` 是 Assembly 关键字，但也可以是一个部件名（memref.global 的 $type），
 * 所以这里接受它；`type(x)` 有括号，`type,`/`type)` 没有，LR(1) 可区分。 */
custom_arg_list
  : custom_arg
  | custom_arg_list COMMA custom_arg
  ;

custom_arg
  : IDENT
  | A_TYPE                        /* 部件名恰好叫 type */
  | A_TYPE L_PAREN IDENT R_PAREN  /* 派生：把某个部件的类型交给钩子 */
  ;

/* oilist(a | b | c)：顺序无关的可选组，每支 0 或 1 次（可以同时出现两支）。
 * 分支用 `|` 分隔，与类型并集、enum 组合子同一个析取符号。ODS 原义见
 * mlir/docs/DefiningDialects/Operations.md:808-819；13 章 §5.6 有完整文法。 */
oilist_alt_list
  : asm_list
  | oilist_alt_list PIPE asm_list
  ;

/*===--------------------------------------------------------------------===*/
/* enum                                                                  */
/*===--------------------------------------------------------------------===*/

enum_decl
  : KW_ENUM name enum_kind_opt L_BRACE enum_item_list R_BRACE
  ;

enum_kind_opt
  : %empty
  | COLON KW_BITS
  ;

enum_item_list
  : enum_item
  | enum_item_list enum_item
  ;

enum_item
  : doc_clause
  | KW_CASES L_BRACKET enum_case_list R_BRACKET SEMI
  | KW_GROUPS L_BRACKET enum_group R_BRACKET SEMI
  ;

enum_case_list
  : enum_case
  | enum_case_list COMMA enum_case
  ;

enum_case
  : IDENT EQUAL INT spelling_opt
  ;

spelling_opt
  : %empty
  | STRING
  ;

/* 一个 Groups 子句一个组；多个组就写多个 Groups 子句。
 * 成员用 `|` 连接，避免与逗号列表的二义性。 */
enum_group
  : IDENT EQUAL name_group
  ;

name_group
  : name
  | name_group PIPE name
  ;

/*===--------------------------------------------------------------------===*/
/* attr_def / type_def                                                   */
/*===--------------------------------------------------------------------===*/

attr_type_decl
  : KW_ATTR_DEF name L_BRACE def_item_list R_BRACE
  | KW_TYPE_DEF name L_BRACE def_item_list R_BRACE
  ;

def_item_list
  : def_item
  | def_item_list def_item
  ;

def_item
  : doc_clause
  | KW_PARAMETERS L_PAREN binding_list R_PAREN SEMI
  | KW_ASSEMBLY assembly
  ;

/*===--------------------------------------------------------------------===*/
/* 文档：数据，不是注释                                                  */
/*===--------------------------------------------------------------------===*/

doc_clause
  : KW_DOC STRING SEMI
  ;

%%
