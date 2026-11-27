/*===-- dialect-dsl.y - a DSL for defining MLIR dialects -------*- yacc -*-===//
 *
 * 归档：v1 是"设计先行"的原型——先定文法再写样例。14 章判定它把
 * codegen 的偶然结构（Namespace/Class/Builders/Extra/Verify code）当成了
 * 语言结构，因此只保留其中的 format 子语言设计，其余不再演进。
 * 现存样例：design-first-v1/examples/*.mdl（注意：这些样例是按文法裁剪的）。
 *
 * 与 TD 的差别（刻意去掉的噪音）：
 *   - 没有 def/class/let：声明就是声明，赋值就是赋值。
 *   - 没有位置参数：op/family 的继承与参数全部具名。
 *   - 没有 mixin：operands/results/attrs 只有一种写法。
 *   - 没有 # / !listconcat：traits 在子类型里追加，不写拼接表达式。
 *   - 没有 $ 与反引号：format 里的变量就是声明的名字，字面量是字符串。
 *
 * 词法模式：普通模式 / format 模式 / code 模式。
 *   - format 模式由 `format {` 进入，`}` 退出；`[ ] < > { } ,` 是结构符，
 *     STRING 是字面量，IDENT 是变量，其余关键字是指令。
 *   - code 模式由 `code {` 或 `code "` 进入，交给 C++ 片段扫描器。
 *
 * 验证：bison -Wall -Wcounterexamples ana-docs/mlir/design-first-v1/dialect-dsl.y
 *===----------------------------------------------------------------------===*/

%start module

%token IDENT STRING INT DOC CODE
%token L_BRACE R_BRACE L_PAREN R_PAREN L_BRACKET R_BRACKET L_ANGLE R_ANGLE
%token COMMA COLON SEMI EQUAL DOT PIPE

/* 声明 */
%token KW_DIALECT KW_OP KW_FAMILY KW_ATTR_DEF KW_TYPE_DEF KW_INTERFACE_DEF
%token KW_ENUM_DEF KW_CONSTRAINT
%token KW_EXTENDS KW_BUILD

/* 方言属性 */
%token KW_NAMESPACE KW_CLASS KW_DEPENDENT

/* op/family 子句 */
%token KW_OPERANDS KW_RESULTS KW_REGIONS KW_SUCCESSORS
%token KW_ATTRS KW_PROPS KW_TRAITS KW_IMPLEMENTS
%token KW_FORMAT KW_CUSTOM KW_BUILDERS KW_VERIFY KW_FOLD KW_CANONICALIZE
%token KW_SUMMARY KW_EXTRA KW_MNEMONIC KW_PARAMETERS KW_METHODS
%token KW_METHOD

/* 类型与值 */
%token KW_OPTIONAL KW_VARIADIC KW_VARIADIC_OF_VARIADIC KW_UNIT
%token KW_TRUE KW_FALSE KW_IF KW_ELSE

/* format 指令 */
%token F_ATTR_DICT F_PROP_DICT F_TYPE F_FUNCTIONAL_TYPE
%token F_OPERANDS F_RESULTS F_REGIONS F_SUCCESSORS
%token F_CUSTOM F_REF F_ENUM F_QUALIFIED F_OILIST F_PARAMS F_STRUCT

%left PIPE

%define parse.trace

%code provides {
int yylex(void);
void yyerror(const char *);
}

%%

/*===--------------------------------------------------------------------===*/
/* 模块与声明                                                            */
/*===--------------------------------------------------------------------===*/

module
  : top_decl_list_opt
  ;

top_decl_list_opt
  : %empty
  | top_decl_list
  ;

top_decl_list
  : top_decl
  | top_decl_list top_decl
  ;

/* 紧邻声明的 /// 注释挂到该声明上；首段为 summary，其余为 description。 */
top_decl
  : doc_list_opt decl
  ;

doc_list_opt
  : %empty
  | doc_list
  ;

doc_list
  : DOC
  | doc_list DOC
  ;

decl
  : dialect_decl
  | op_decl
  | family_decl
  | attr_decl
  | type_decl
  | interface_decl
  | enum_decl
  | constraint_decl
  ;

/*===--------------------------------------------------------------------===*/
/* dialect                                                              */
/*===--------------------------------------------------------------------===*/

dialect_decl
  : KW_DIALECT IDENT L_BRACE dialect_item_list_opt R_BRACE
  ;

dialect_item_list_opt
  : %empty
  | dialect_item_list
  ;

dialect_item_list
  : dialect_item
  | dialect_item_list dialect_item
  ;

dialect_item
  : dialect_prop
  | doc_list_opt dialect_member
  ;

dialect_prop
  : KW_NAMESPACE STRING SEMI
  | KW_CLASS STRING SEMI
  | KW_DEPENDENT L_BRACKET ident_list_opt R_BRACKET SEMI
  ;

dialect_member
  : op_decl
  | family_decl
  | attr_decl
  | type_decl
  | interface_decl
  | enum_decl
  | constraint_decl
  ;

ident_list_opt
  : %empty
  | ident_list
  ;

ident_list
  : IDENT
  | ident_list COMMA IDENT
  ;

/*===--------------------------------------------------------------------===*/
/* op / family                                                          */
/*                                                                      */
/*   op AddIOp "addi" extends IntBinary<overflow> { ... }               */
/*   family IntBinary<flag: flag = none> extends Binary { ... }         */
/*===--------------------------------------------------------------------===*/

op_decl
  : KW_OP IDENT STRING extends_clause_opt L_BRACE op_item_list_opt R_BRACE
  ;

family_decl
  : KW_FAMILY IDENT type_params_opt extends_clause_opt
    L_BRACE op_item_list_opt R_BRACE
  ;

extends_clause_opt
  : %empty
  | KW_EXTENDS type_ref
  ;

type_ref
  : IDENT type_args_opt
  ;

type_args_opt
  : %empty
  | L_ANGLE expr_list_opt R_ANGLE
  ;

type_params_opt
  : %empty
  | type_params
  ;

type_params
  : L_ANGLE param_list R_ANGLE
  ;

param_list
  : param
  | param_list COMMA param
  ;

/* 模板参数是"元类型"：string / type / [trait] / flag ... */
param
  : IDENT COLON param_type opt_default
  ;

param_type
  : type_expr
  | L_BRACKET type_expr R_BRACKET
  ;

opt_default
  : %empty
  | EQUAL expr
  ;

/* 块状子句（code {...}）自带边界，分号可选，允许两种书写风格。 */
opt_semi
  : %empty
  | SEMI
  ;

/*===--------------------------------------------------------------------===*/
/* op/family 子句：一种概念只有一种写法                                   */
/*===--------------------------------------------------------------------===*/

op_item_list_opt
  : %empty
  | op_item_list
  ;

op_item_list
  : op_item
  | op_item_list op_item
  ;

op_item
  : KW_OPERANDS L_PAREN binding_list_opt R_PAREN
  | KW_RESULTS L_PAREN binding_list_opt R_PAREN
  | KW_REGIONS L_PAREN binding_list_opt R_PAREN
  | KW_SUCCESSORS L_PAREN binding_list_opt R_PAREN
  | KW_ATTRS L_PAREN attr_binding_list_opt R_PAREN
  | KW_PROPS L_PAREN attr_binding_list_opt R_PAREN
  | KW_TRAITS L_BRACKET type_ref_list_opt R_BRACKET
  | KW_IMPLEMENTS L_BRACKET type_ref_list_opt R_BRACKET
  | KW_FORMAT format_block
  | KW_FORMAT KW_CUSTOM SEMI
  | KW_BUILDERS L_BRACKET builder_list_opt R_BRACKET
  | KW_VERIFY SEMI
  | KW_VERIFY CODE opt_semi
  | KW_VERIFY STRING SEMI
  | KW_FOLD SEMI
  | KW_FOLD CODE opt_semi
  | KW_FOLD STRING SEMI
  | KW_CANONICALIZE SEMI
  | KW_CANONICALIZE CODE opt_semi
  | KW_CANONICALIZE STRING SEMI
  | KW_SUMMARY STRING SEMI
  | KW_EXTRA CODE opt_semi
  | KW_EXTRA STRING SEMI
  ;

binding_list_opt
  : %empty
  | binding_list
  ;

binding_list
  : binding
  | binding_list COMMA binding
  ;

binding
  : IDENT COLON type_expr
  ;

attr_binding_list_opt
  : %empty
  | attr_binding_list
  ;

attr_binding_list
  : attr_binding
  | attr_binding_list COMMA attr_binding
  ;

/* name: Type          必选
 * name: Type = value  有默认值
 * name: Type optional 可选
 * name: unit          单位属性
 */
attr_binding
  : IDENT COLON type_expr attr_mode_opt
  ;

attr_mode_opt
  : %empty
  | EQUAL expr
  | KW_OPTIONAL
  ;

type_ref_list_opt
  : %empty
  | type_ref_list
  ;

type_ref_list
  : type_ref
  | type_ref_list COMMA type_ref
  ;

/*===--------------------------------------------------------------------===*/
/* 类型表达式                                                            */
/*===--------------------------------------------------------------------===*/

type_expr
  : type_primary
  | type_expr PIPE type_expr
  ;

type_primary
  : IDENT type_args_angle_opt
  | KW_OPTIONAL L_ANGLE type_expr R_ANGLE
  | KW_VARIADIC L_ANGLE type_expr R_ANGLE
  | KW_VARIADIC_OF_VARIADIC L_ANGLE type_expr COMMA STRING R_ANGLE
  | KW_UNIT
  ;

type_args_angle_opt
  : %empty
  | L_ANGLE type_arg_list_opt R_ANGLE
  ;

type_arg_list_opt
  : %empty
  | type_arg_list
  ;

/* 类型实参既可以是类型，也可以是整数字面量（如区域大小 SizedRegion<1>）。 */
type_arg
  : type_expr
  | INT
  ;

type_arg_list
  : type_arg
  | type_arg_list COMMA type_arg
  ;

/*===--------------------------------------------------------------------===*/
/* 值表达式：默认值、模板实参、builder 默认值                            */
/*===--------------------------------------------------------------------===*/

expr
  : STRING
  | INT
  | KW_TRUE
  | KW_FALSE
  | IDENT expr_postfix
  | L_BRACKET expr_list_opt R_BRACKET
  | L_PAREN expr R_PAREN
  ;

expr_postfix
  : %empty
  | expr_postfix DOT IDENT
  | expr_postfix L_ANGLE expr_list_opt R_ANGLE
  ;

expr_list_opt
  : %empty
  | expr_list
  ;

expr_list
  : expr
  | expr_list COMMA expr
  ;

/*===--------------------------------------------------------------------===*/
/* builders                                                             */
/*   build(lhs: Value, rhs: Value, fastmath: FastMathFlags = none)       */
/*         code { ... }                                                 */
/*===--------------------------------------------------------------------===*/

builder_list_opt
  : %empty
  | builder_list
  ;

builder_list
  : builder
  | builder_list COMMA builder
  ;

builder
  : KW_BUILD L_PAREN builder_param_list_opt R_PAREN code_opt
  ;

builder_param_list_opt
  : %empty
  | builder_param_list
  ;

builder_param_list
  : builder_param
  | builder_param_list COMMA builder_param
  ;

builder_param
  : IDENT COLON cxx_type builder_default_opt
  ;

builder_default_opt
  : %empty
  | EQUAL expr
  ;

cxx_type
  : IDENT cxx_args_opt
  ;

cxx_args_opt
  : %empty
  | L_ANGLE expr_list_opt R_ANGLE
  ;

/*===--------------------------------------------------------------------===*/
/* 代码逃逸：显式、带边界                                                 */
/*===--------------------------------------------------------------------===*/

code_opt
  : %empty
  | code_value
  ;

code_value
  : CODE
  | STRING
  ;

/*===--------------------------------------------------------------------===*/
/* format：一等语法，不是字符串                                          */
/*                                                                      */
/*   format { lhs "," rhs if overflow { "overflow" overflow }           */
/*            attr-dict ":" type[result] }                              */
/*===--------------------------------------------------------------------===*/

format_block
  : L_BRACE format_element_list_opt R_BRACE
  ;

format_element_list_opt
  : %empty
  | format_element_list
  ;

format_element_list
  : format_element
  | format_element_list format_element
  ;

format_element
  : STRING                                   /* 字面量，含标点："," ":" */
  | IDENT                                    /* 引用声明的名字 */
  | F_ATTR_DICT
  | F_PROP_DICT
  | F_OPERANDS
  | F_RESULTS
  | F_REGIONS
  | F_SUCCESSORS
  | F_TYPE L_BRACKET format_type_arg R_BRACKET
  | F_FUNCTIONAL_TYPE L_BRACKET format_type_arg COMMA format_type_arg R_BRACKET
  | F_ENUM L_BRACKET IDENT R_BRACKET
  | F_QUALIFIED L_BRACKET format_qualified_arg R_BRACKET
  | F_REF L_BRACKET format_ref_arg R_BRACKET
  | F_PARAMS
  | F_STRUCT L_BRACKET format_element_list_opt R_BRACKET
  | F_CUSTOM L_ANGLE IDENT R_ANGLE
    L_BRACKET format_custom_arg_list_opt R_BRACKET
  | F_OILIST oilist_sep_opt L_BRACKET format_oilist_clause_list R_BRACKET
  | format_conditional
  ;

format_type_arg
  : IDENT
  | F_OPERANDS
  | F_RESULTS
  ;

format_qualified_arg
  : IDENT
  | F_TYPE L_BRACKET format_type_arg R_BRACKET
  ;

format_ref_arg
  : IDENT
  | F_ATTR_DICT
  | F_PROP_DICT
  | F_OPERANDS
  | F_RESULTS
  | F_REGIONS
  | F_SUCCESSORS
  | F_TYPE L_BRACKET format_type_arg R_BRACKET
  ;

format_custom_arg_list_opt
  : %empty
  | format_custom_arg_list
  ;

format_custom_arg_list
  : format_custom_arg
  | format_custom_arg_list COMMA format_custom_arg
  ;

format_custom_arg
  : STRING
  | IDENT
  | F_ATTR_DICT
  | F_PROP_DICT
  | F_REF L_BRACKET format_ref_arg R_BRACKET
  | F_TYPE L_BRACKET format_type_arg R_BRACKET
  | F_QUALIFIED L_BRACKET format_qualified_arg R_BRACKET
  ;

/* 可选组：条件即锚点，语义比 ^ 更直接。
 * 锚点可以是某个具名变量，也可以是 operands/results/regions/successors
 * 这类"整组"指令（对应 TD 里对 variadic 组求非空的写法）。
 */
format_conditional
  : KW_IF format_anchor L_BRACE format_element_list_opt R_BRACE format_else_opt
  ;

format_anchor
  : IDENT
  | F_OPERANDS
  | F_RESULTS
  | F_REGIONS
  | F_SUCCESSORS
  | F_ATTR_DICT
  | F_PROP_DICT
  ;

format_else_opt
  : %empty
  | KW_ELSE L_BRACE format_element_list_opt R_BRACE
  ;

oilist_sep_opt
  : %empty
  | L_ANGLE STRING R_ANGLE
  ;

format_oilist_clause_list
  : format_oilist_clause
  | format_oilist_clause_list PIPE format_oilist_clause
  ;

format_oilist_clause
  : STRING format_element_list_opt
  ;

/*===--------------------------------------------------------------------===*/
/* attr / type / interface / enum / constraint                          */
/*===--------------------------------------------------------------------===*/

attr_decl
  : KW_ATTR_DEF IDENT opt_supertype L_BRACE def_item_list_opt R_BRACE
  ;

type_decl
  : KW_TYPE_DEF IDENT opt_supertype L_BRACE def_item_list_opt R_BRACE
  ;

opt_supertype
  : %empty
  | COLON type_ref
  ;

def_item_list_opt
  : %empty
  | def_item_list
  ;

def_item_list
  : def_item
  | def_item_list def_item
  ;

def_item
  : KW_PARAMETERS L_PAREN attr_binding_list_opt R_PAREN
  | KW_FORMAT format_block
  | KW_FORMAT KW_CUSTOM SEMI
  | KW_MNEMONIC STRING SEMI
  | KW_SUMMARY STRING SEMI
  | KW_TRAITS L_BRACKET type_ref_list_opt R_BRACKET
  | KW_VERIFY SEMI
  | KW_VERIFY CODE opt_semi
  | KW_VERIFY STRING SEMI
  | KW_EXTRA CODE opt_semi
  | KW_EXTRA STRING SEMI
  ;

interface_decl
  : KW_INTERFACE_DEF IDENT opt_supertype L_BRACE interface_item_list_opt R_BRACE
  ;

interface_item_list_opt
  : %empty
  | interface_item_list
  ;

interface_item_list
  : interface_item
  | interface_item_list interface_item
  ;

interface_item
  : KW_NAMESPACE STRING SEMI
  | KW_SUMMARY STRING SEMI
  | KW_METHODS L_BRACKET method_list_opt R_BRACKET
  | KW_EXTENDS L_BRACKET type_ref_list_opt R_BRACKET
  | KW_EXTRA CODE opt_semi
  | KW_EXTRA STRING SEMI
  ;

method_list_opt
  : %empty
  | method_list
  ;

method_list
  : method
  | method_list COMMA method
  ;

method
  : KW_METHOD IDENT L_PAREN builder_param_list_opt R_PAREN code_opt
  ;

enum_decl
  : KW_ENUM_DEF IDENT COLON type_expr L_BRACE enum_case_list_opt R_BRACE
  ;

enum_case_list_opt
  : %empty
  | enum_case_list
  ;

enum_case_list
  : enum_case
  | enum_case_list enum_case
  ;

enum_case
  : IDENT EQUAL INT opt_string SEMI
  ;

opt_string
  : %empty
  | COMMA STRING
  ;

constraint_decl
  : KW_CONSTRAINT IDENT EQUAL type_expr SEMI
  ;

%%
