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
 *   - 名字允许 `.`，因此 `reduce.return`、`forall.in_parallel` 无需转义；
 *   - `type`/`if`/`attr-dict` 只在 Assembly 模式里是关键字，
 *     所以 `op if`、`type_def type` 在普通模式里都合法。
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

/* Assembly 模式 */
%token A_ATTR_DICT A_TYPE A_IF

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

/* `reduce.return`、`forall.in_parallel`：点号属于名字。 */
name
  : IDENT
  | name DOT IDENT
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

type_arg_list
  : type_expr
  | type_arg_list COMMA type_expr
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
  : IDENT COLON type_expr
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
  | KW_REGIONS L_PAREN binding_list R_PAREN SEMI
  | KW_SUCCESSORS L_PAREN binding_list R_PAREN SEMI
  | KW_PROPERTIES L_BRACKET fact_list R_BRACKET SEMI
  | KW_IMPLEMENTS L_BRACKET type_ref_list R_BRACKET SEMI
  | KW_ASSEMBLY assembly
  ;

binding_list
  : binding
  | binding_list COMMA binding
  ;

binding
  : IDENT COLON type_expr default_opt
  ;

default_opt
  : %empty
  | EQUAL expr
  ;

/* 语料里的默认值只有 `none` 这类标识符；没有字符串/整数默认值，
 * 所以这里不写 STRING/INT 分支（写了就是无见证产生式）。 */
expr
  : IDENT
  ;

/* 语义事实：pure / terminator / has_parent(func) / parent_one_of(a, b) */
fact_list
  : fact
  | fact_list COMMA fact
  ;

fact
  : IDENT
  | IDENT L_PAREN name_list R_PAREN
  ;

name_list
  : name
  | name_list COMMA name
  ;

/*===--------------------------------------------------------------------===*/
/* Assembly：块状双向程序，或整体交给钩子                                */
/*===--------------------------------------------------------------------===*/

assembly
  : L_BRACE asm_list R_BRACE
  | KW_CUSTOM SEMI
  ;

asm_list
  : asm_element
  | asm_list asm_element
  ;

asm_element
  : STRING                       /* 字面量，含标点 */
  | IDENT                        /* 引用一个已声明的部件 */
  | A_ATTR_DICT                  /* 剩余属性集合 */
  | A_TYPE L_PAREN IDENT R_PAREN /* 打印某个部件的类型 */
  | A_IF IDENT L_BRACE asm_list R_BRACE
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
