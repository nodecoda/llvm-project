/*===-- ods.y - ODS semantic grammar (record layer) ------------*- yacc -*-===//
 *
 * 这不是 TableGen 宿主语法，也没有任何 parser 执行它。
 *
 * TableGen 的 TGParser 只看到 def / class / let / 标识符 / 字符串；
 * ODS 的语义（"这是一个 Op"、"这个 dag 是 arguments"、"这个 class 是约束"）
 * 是 mlir-tblgen 的 ODS backend 在遍历 Record 时*识别*出来的
 * （mlir/lib/TableGen/Operator.cpp、mlir/tools/mlir-tblgen/OpDefinitionsGen.cpp）。
 *
 * 本文件把那个被识别出来的结构形式化：终结符是 ODS 语义关键字，
 * 而不是 TD 关键字。文法的价值是"可审阅的规范 + 合法性分析"，
 * 不是"可运行的解析器"。
 *
 * 对应的 TD 写法与逐节解释见 ana-docs/mlir/13-ods-semantic-grammar.md §4/§6。
 * 验证：bison -Wall -Wcounterexamples ana-docs/mlir/ods.y
 *===----------------------------------------------------------------------===*/

%start ods_file

/* ---- 基础词法（在 TD 层全部由 TGParser 的 lexer 产生） ---- */
%token IDENTIFIER STRING_LITERAL CODE_FRAGMENT INTEGER VARIABLE
%token L_PAREN R_PAREN L_ANGLE R_ANGLE L_BRACE R_BRACE L_BRACKET R_BRACKET
%token COMMA COLON SCOPE DOT EQUAL SEMI QUESTION
%token CONCAT BANG

/* ---- dag 的 operator：ins/outs/region/successor 是 marker def ---- */
%token KW_INS KW_OUTS KW_REGION KW_SUCCESSOR
/* Arguments<...> / Results<...> mixin（OpBase.td:479/484） */
%token KW_ARGUMENTS KW_RESULTS

/* ---- 实体关键字：语义 kind，由基类链决定，不是 TD 关键字 ---- */
%token KW_DIALECT KW_OP KW_OP_FAMILY KW_ATTR_DEF KW_TYPE_DEF
%token KW_INTERFACE KW_OP_INTERFACE KW_ATTR_INTERFACE
%token KW_TYPE_INTERFACE KW_DIALECT_INTERFACE
%token KW_I32_ENUM_ATTR KW_I64_ENUM_ATTR KW_BIT_ENUM_ATTR
%token KW_I32_ENUM_CASE KW_I64_ENUM_CASE KW_BIT_ENUM_CASE
%token KW_PASS KW_PAT KW_TYPE_CONSTRAINT KW_ATTR_CONSTRAINT KW_PROPERTY_DEF

/* ---- 字段名：语义层字段，TD 层是 let 后面的标识符 ---- */
%token F_ARGUMENTS F_RESULTS F_REGIONS F_SUCCESSORS F_PARAMETERS
%token F_ASSEMBLY_FORMAT F_TRAITS F_BASE_INTERFACES
%token F_BUILDERS F_OPTIONS F_METHODS F_CPP_INCLUDES
%token F_SUMMARY F_DESCRIPTION F_CPP_NAMESPACE F_MNEMONIC F_PARTITION
%token F_EXTRA_CLASS_DECL F_EXTRA_CLASS_DEF F_EXTRA_SHARED_CLASS_DECL
%token F_EXTRA_CLASS_OF F_CONSTRUCTOR

/* ---- let 的合并模式（ProgRef.md:740） ---- */
%token KW_APPEND KW_PREPEND

/* ---- 约束表达式 ---- */
%token CONSTRAINT_ATOM                    /* I32 / Index / AnyInteger / ... */
%token KW_ANY_TYPE_OF KW_ALL_OF_TYPE KW_AND KW_OR KW_NEG
%token KW_OPTIONAL KW_VARIADIC KW_VARIADIC_OF_VARIADIC KW_CPRED KW_PROPERTY

/* ---- OpBuilder<...> ---- */
%token KW_OP_BUILDER

%%

/*===--------------------------------------------------------------------===*/
/* 一个 ODS 文件 = 一组语义实体。include 已在 TD 层展开，这里不建模。       */
/*===--------------------------------------------------------------------===*/

ods_file
  : %empty
  | ods_file ods_entity
  ;

ods_entity
  : dialect_def
  | op_def
  | op_family_def
  | attr_or_type_def
  | interface_def
  | enum_def
  | pass_def
  | pattern_def
  | property_def
  | constraint_def
  ;

opt_semi
  : %empty
  | SEMI
  ;

/* 所有实体共享的 { let field = value; ... } 体。 */
entity_body_opt
  : %empty
  | L_BRACE field_list R_BRACE
  ;

/*===--------------------------------------------------------------------===*/
/* Dialect：字段驱动，name/cppNamespace/dependentDialects/... 都是字段。    */
/*===--------------------------------------------------------------------===*/

dialect_def
  : KW_DIALECT entity_body_opt opt_semi
  ;

/*===--------------------------------------------------------------------===*/
/* Op 与 OpFamily                                                        */
/*                                                                       */
/* TD 里有三种等价写法，本文法用 op_suffix_list + entity_body 统一表达：    */
/*   1) MI 式：  def X : Arith_Op<"x", [T]>, Arguments<(ins ...)>, ...;  */
/*   2) body 式：def X : Arith_Op<"x"> { let arguments = ...; }           */
/*   3) family： class X<...> : Arith_Op<...> { let arguments = ...; }    */
/* 1/2 的差别只是 Arguments<...> 出现在后缀还是 body；3 多了模板参数，      */
/* 所以单列为 OpFamily（语义 kind，TD 层写作 class）。                     */
/*===--------------------------------------------------------------------===*/

op_def
  : op_header op_suffix_list entity_body_opt opt_semi
  ;

op_header
  : KW_OP L_ANGLE dialect_ref COMMA STRING_LITERAL R_ANGLE
  ;

dialect_ref
  : qualified_ident
  ;

/* 第 3 个模板参数（traits）与 mixin 都以 COMMA 开头，故合并成一个后缀列表。 */
op_suffix_list
  : %empty
  | op_suffix_list op_suffix
  ;

op_suffix
  : COMMA list_value                                              /* [T1, T2] */
  | COMMA KW_ARGUMENTS L_ANGLE L_PAREN KW_INS dag_arg_list_opt R_PAREN R_ANGLE
  | COMMA KW_RESULTS L_ANGLE L_PAREN KW_OUTS dag_arg_list_opt R_PAREN R_ANGLE
  ;

op_family_def
  : KW_OP_FAMILY L_ANGLE param_decl_list_opt R_ANGLE
    COLON family_base op_suffix_list entity_body_opt opt_semi
  ;

family_base
  : qualified_ident opt_template_args
  ;

param_decl_list_opt
  : %empty
  | param_decl_list
  ;

param_decl_list
  : param_decl
  | param_decl_list COMMA param_decl
  ;

param_decl
  : qualified_ident opt_template_args IDENTIFIER opt_default
  ;

opt_default
  : %empty
  | EQUAL value
  ;

/*===--------------------------------------------------------------------===*/
/* AttrDef / TypeDef：kind 由基类决定（AttrDef vs TypeDef）               */
/*===--------------------------------------------------------------------===*/

attr_or_type_def
  : attr_or_type_kind L_ANGLE dialect_ref COMMA STRING_LITERAL
    attr_type_tail R_ANGLE entity_body_opt opt_semi
  ;

attr_or_type_kind
  : KW_ATTR_DEF
  | KW_TYPE_DEF
  ;

/* 第 3/4 个位置参数：traits 列表、C++ 基类字符串。 */
attr_type_tail
  : %empty
  | COMMA list_value
  | COMMA list_value COMMA STRING_LITERAL
  ;

/*===--------------------------------------------------------------------===*/
/* Interface：OpInterface / AttrInterface / TypeInterface / ...           */
/*===--------------------------------------------------------------------===*/

interface_def
  : interface_kind L_ANGLE STRING_LITERAL opt_base_interfaces R_ANGLE
    entity_body_opt opt_semi
  ;

interface_kind
  : KW_INTERFACE
  | KW_OP_INTERFACE
  | KW_ATTR_INTERFACE
  | KW_TYPE_INTERFACE
  | KW_DIALECT_INTERFACE
  ;

opt_base_interfaces
  : %empty
  | COMMA list_value
  ;

/*===--------------------------------------------------------------------===*/
/* Enum：cases 是模板参数，不是 body 字段（EnumAttr.td:347/435）           */
/*===--------------------------------------------------------------------===*/

enum_def
  : enum_kind L_ANGLE enum_params R_ANGLE entity_body_opt opt_semi
  ;

enum_kind
  : KW_I32_ENUM_ATTR
  | KW_I64_ENUM_ATTR
  | KW_BIT_ENUM_ATTR
  ;

enum_params
  : STRING_LITERAL COMMA STRING_LITERAL COMMA enum_case_list
  | qualified_ident COMMA STRING_LITERAL COMMA STRING_LITERAL
    COMMA enum_case_list
  ;

enum_case_list
  : %empty
  | enum_case
  | enum_case_list COMMA enum_case
  ;

enum_case
  : enum_case_kind L_ANGLE STRING_LITERAL COMMA INTEGER opt_string R_ANGLE
  ;

enum_case_kind
  : KW_I32_ENUM_CASE
  | KW_I64_ENUM_CASE
  | KW_BIT_ENUM_CASE
  ;

opt_string
  : %empty
  | COMMA STRING_LITERAL
  ;

/*===--------------------------------------------------------------------===*/
/* Pass / Pattern / Property / Constraint                                */
/*===--------------------------------------------------------------------===*/

pass_def
  : KW_PASS L_ANGLE STRING_LITERAL opt_pass_operation R_ANGLE
    entity_body_opt opt_semi
  ;

opt_pass_operation
  : %empty
  | COMMA STRING_LITERAL
  ;

pattern_def
  : KW_PAT L_ANGLE value_list_opt R_ANGLE opt_semi
  ;

property_def
  : KW_PROPERTY_DEF L_ANGLE value_list_opt R_ANGLE opt_semi
  ;

constraint_def
  : constraint_kind L_ANGLE value_list R_ANGLE entity_body_opt opt_semi
  ;

constraint_kind
  : KW_TYPE_CONSTRAINT
  | KW_ATTR_CONSTRAINT
  ;

/*===--------------------------------------------------------------------===*/
/* body 字段：field_mode 对应 TD 的 let / let append / let prepend        */
/*===--------------------------------------------------------------------===*/

field_list
  : %empty
  | field_list field
  ;

field
  : field_mode field_assign SEMI
  ;

field_mode
  : %empty
  | KW_APPEND
  | KW_PREPEND
  ;

/* summary / description / assemblyFormat 常写成 [{ ... }]。 */
text_value
  : STRING_LITERAL
  | CODE_FRAGMENT
  ;

field_assign
  /* 结构上有独立语义的 dag 字段 */
  : F_ARGUMENTS EQUAL L_PAREN KW_INS dag_arg_list_opt R_PAREN
  | F_RESULTS EQUAL L_PAREN KW_OUTS dag_arg_list_opt R_PAREN
  | F_REGIONS EQUAL L_PAREN KW_REGION dag_arg_list_opt R_PAREN
  | F_SUCCESSORS EQUAL L_PAREN KW_SUCCESSOR dag_arg_list_opt R_PAREN
  | F_PARAMETERS EQUAL L_PAREN KW_INS dag_arg_list_opt R_PAREN
  /* 列表字段 */
  | F_TRAITS EQUAL list_value
  | F_BASE_INTERFACES EQUAL list_value
  | F_BUILDERS EQUAL list_value
  | F_OPTIONS EQUAL list_value
  | F_METHODS EQUAL list_value
  | F_CPP_INCLUDES EQUAL list_value
  /* 字符串 / 代码片段字段 */
  | F_ASSEMBLY_FORMAT EQUAL text_value
  | F_SUMMARY EQUAL text_value
  | F_DESCRIPTION EQUAL text_value
  | F_CPP_NAMESPACE EQUAL STRING_LITERAL
  | F_MNEMONIC EQUAL STRING_LITERAL
  | F_EXTRA_CLASS_DECL EQUAL CODE_FRAGMENT
  | F_EXTRA_CLASS_DEF EQUAL CODE_FRAGMENT
  | F_EXTRA_SHARED_CLASS_DECL EQUAL CODE_FRAGMENT
  | F_EXTRA_CLASS_OF EQUAL CODE_FRAGMENT
  | F_CONSTRUCTOR EQUAL CODE_FRAGMENT
  | F_PARTITION EQUAL value
  /* 其余字段（bit 标志、storageClass、description ...）语义同构 */
  | IDENTIFIER EQUAL value
  ;

/*===--------------------------------------------------------------------===*/
/* 值：TD 的 Init 树的语义视图                                          */
/*===--------------------------------------------------------------------===*/

/* TD 的 Init 表达式：`#` 连接、`.` 取字段、`!op(...)` bang 算子。 */
value
  : concat_expr
  ;

concat_expr
  : postfix_value
  | concat_expr CONCAT postfix_value
  ;

postfix_value
  : primary_value
  | postfix_value DOT IDENTIFIER
  ;

primary_value
  : STRING_LITERAL
  | INTEGER
  | CODE_FRAGMENT
  | QUESTION
  | type_constraint
  | dag_value
  | list_value
  | ctor_value
  | builder_value
  | bang_value
  ;

/* !if(...) / !empty(...) / !strconcat(...) / !foreach(...) 等 */
bang_value
  : BANG IDENTIFIER L_PAREN value_list_opt R_PAREN
  ;

dag_value
  : L_PAREN dag_op dag_arg_list_opt R_PAREN
  ;

dag_op
  : KW_INS
  | KW_OUTS
  | KW_REGION
  | KW_SUCCESSOR
  | qualified_ident
  ;

dag_arg_list_opt
  : %empty
  | dag_arg_list
  ;

dag_arg_list
  : dag_arg
  | dag_arg_list COMMA dag_arg
  ;

dag_arg
  : value opt_arg_name
  ;

opt_arg_name
  : %empty
  | COLON VARIABLE
  ;

list_value
  : L_BRACKET value_list_opt R_BRACKET
  ;

value_list_opt
  : %empty
  | value_list
  ;

value_list
  : value
  | value_list COMMA value
  ;

/* Class<...> / DeclareOpInterfaceMethods<...> / SizedRegion<1> ... */
ctor_value
  : qualified_ident L_ANGLE value_list_opt R_ANGLE
  ;

opt_template_args
  : %empty
  | L_ANGLE value_list_opt R_ANGLE
  ;

qualified_ident
  : IDENTIFIER
  | qualified_ident SCOPE IDENTIFIER
  ;

/* OpBuilder<(ins "Value":$lhs), [{ ... }]> */
builder_value
  : KW_OP_BUILDER L_ANGLE L_PAREN builder_ins_list_opt R_PAREN
    builder_body_opt R_ANGLE
  ;

builder_ins_list_opt
  : %empty
  | builder_ins_list
  ;

builder_ins_list
  : builder_ins
  | builder_ins_list COMMA builder_ins
  ;

builder_ins
  : STRING_LITERAL COLON VARIABLE
  ;

builder_body_opt
  : %empty
  | COMMA CODE_FRAGMENT
  | COMMA STRING_LITERAL
  ;

/*===--------------------------------------------------------------------===*/
/* 约束表达式                                                        */
/*                                                                   */
/* 注意：And/Or/Neg 作用于 Pred, AnyTypeOf/AllOfType 作用于 Type。      */
/* 这个区分是语义检查，不是语法检查（见文档 §7）。                      */
/*===--------------------------------------------------------------------===*/

type_constraint
  : CONSTRAINT_ATOM
  | qualified_ident
  | pred_expr
  | KW_ANY_TYPE_OF L_ANGLE list_value R_ANGLE
  | KW_ALL_OF_TYPE L_ANGLE list_value R_ANGLE
  | KW_OPTIONAL L_ANGLE type_constraint R_ANGLE
  | KW_VARIADIC L_ANGLE type_constraint R_ANGLE
  | KW_VARIADIC_OF_VARIADIC L_ANGLE type_constraint COMMA STRING_LITERAL R_ANGLE
  | KW_PROPERTY L_ANGLE value_list_opt R_ANGLE
  ;

pred_expr
  : KW_CPRED L_ANGLE STRING_LITERAL R_ANGLE
  | KW_CPRED L_ANGLE STRING_LITERAL COMMA STRING_LITERAL R_ANGLE
  | KW_AND L_ANGLE list_value R_ANGLE
  | KW_OR L_ANGLE list_value R_ANGLE
  | KW_NEG L_ANGLE pred_expr R_ANGLE
  ;

%%
