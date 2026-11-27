/*===-- ods-asm-format.y - ODS assemblyFormat sub-language -----*- yacc -*-===//
 *
 * assemblyFormat 的值在 TD 层是一个 STRING_LITERAL；mlir-tblgen 会把这个字符串
 * 再解析一次，得到 FormatElement 树。实现在：
 *   mlir/tools/mlir-tblgen/FormatGen.cpp      （词法 + 通用结构）
 *   mlir/tools/mlir-tblgen/OpFormatGen.cpp    （Op 的指令与语义检查）
 *   mlir/tools/mlir-tblgen/AttrOrTypeFormatGen.cpp（params/struct）
 *
 * 本文件把那次"字符串内二次解析"形式化。它是真正的递归下降解析器所接受的
 * 语言（上下文由不同 nonterminal 表达），没有 parser 执行这份 yacc 文本。
 *
 * 文档：ana-docs/mlir/13-ods-semantic-grammar.md §5
 * 验证：bison -Wall -Wcounterexamples ana-docs/mlir/ods-asm-format.y
 *===----------------------------------------------------------------------===*/

%start format

/* ---- 结构符号（FormatGen.h:40-56） ---- */
%token L_PAREN R_PAREN L_ANGLE R_ANGLE
%token COLON COMMA CARET QUESTION PIPE

/* ---- 带值的 token（FormatGen.h:78-82） ---- */
%token LITERAL            /* `text`，含 `` 与 `\n`（WhitespaceElement） */
%token VARIABLE           /* $name */
%token STRING_LITERAL     /* "text" */
%token IDENTIFIER          /* custom<name> 的 name */

/* ---- 关键字（FormatGen.h:58-76） ---- */
%token KW_ATTR_DICT KW_ATTR_DICT_W_KEYWORD KW_PROP_DICT
%token KW_CUSTOM KW_ENUM KW_FUNCTIONAL_TYPE KW_OILIST
%token KW_OPERANDS KW_PARAMS KW_QUALIFIED KW_REF
%token KW_REGIONS KW_RESULTS KW_STRUCT KW_SUCCESSORS KW_TYPE

%%

/*===--------------------------------------------------------------------===*/
/* 顶层：element*                                                        */
/*===--------------------------------------------------------------------===*/

format
  : top_element_list_opt
  ;

top_element_list_opt
  : %empty
  | top_element_list
  ;

top_element_list
  : top_element
  | top_element_list top_element
  ;

top_element
  : literal
  | variable
  | attr_dict
  | prop_dict
  | operands
  | regions
  | results
  | successors
  | type_directive
  | functional_type
  | enum_directive
  | qualified_directive
  | custom_directive
  | oilist_directive
  | params_directive
  | struct_directive
  | optional_group
  ;

/*===--------------------------------------------------------------------===*/
/* 叶子：literal 只允许出现在顶层；string 只允许出现在 custom 参数里        */
/*===--------------------------------------------------------------------===*/

literal
  : LITERAL
  ;

variable
  : VARIABLE
  ;

/*===--------------------------------------------------------------------===*/
/* 单指令                                                                */
/*===--------------------------------------------------------------------===*/

attr_dict
  : KW_ATTR_DICT
  | KW_ATTR_DICT_W_KEYWORD
  ;

prop_dict
  : KW_PROP_DICT
  ;

operands
  : KW_OPERANDS
  ;

regions
  : KW_REGIONS
  ;

results
  : KW_RESULTS
  ;

successors
  : KW_SUCCESSORS
  ;

/* type($x) / type(operands) / type(results) */
type_directive
  : KW_TYPE L_PAREN type_operand R_PAREN
  ;

type_operand
  : variable
  | operands
  | results
  ;

/* functional-type(operands, results) */
functional_type
  : KW_FUNCTIONAL_TYPE L_PAREN type_operand COMMA type_operand R_PAREN
  ;

/* enum($attr)：要求 enum attr；bit enum 必须有 0 值 case（OpFormatGen.cpp:4283） */
enum_directive
  : KW_ENUM L_PAREN variable R_PAREN
  ;

/* qualified($attr) / qualified(type($x)) */
qualified_directive
  : KW_QUALIFIED L_PAREN qualified_arg R_PAREN
  ;

qualified_arg
  : variable
  | type_directive
  ;

/*===--------------------------------------------------------------------===*/
/* custom<name>(args)                                                    */
/*===--------------------------------------------------------------------===*/

custom_directive
  : KW_CUSTOM L_ANGLE custom_name R_ANGLE L_PAREN custom_arg_list_opt R_PAREN
  ;

custom_name
  : IDENTIFIER
  | KW_ENUM                /* custom<enum>(...) 合法（FormatGen.h 注释） */
  ;

custom_arg_list_opt
  : %empty
  | custom_arg_list
  ;

custom_arg_list
  : custom_arg
  | custom_arg_list COMMA custom_arg
  ;

/* verifyCustomDirectiveArguments 只接受这些元素（OpFormatGen.cpp:4366-4382） */
custom_arg
  : STRING_LITERAL
  | variable
  | attr_dict
  | prop_dict
  | ref_directive
  | type_directive
  | qualified_directive
  ;

/* ref(...)：只能出现在 custom 里（FormatGen.cpp:446-459） */
ref_directive
  : KW_REF L_PAREN ref_arg R_PAREN
  ;

ref_arg
  : variable
  | attr_dict
  | operands
  | regions
  | results
  | successors
  | type_directive
  ;

/*===--------------------------------------------------------------------===*/
/* oilist [<sep>] ( `kw` elems | `kw2` elems ... )                       */
/*===--------------------------------------------------------------------===*/

oilist_directive
  : KW_OILIST oilist_separator_opt L_PAREN oilist_clause_list R_PAREN
  ;

oilist_separator_opt
  : %empty
  | L_ANGLE literal R_ANGLE
  ;

oilist_clause_list
  : oilist_clause
  | oilist_clause_list PIPE oilist_clause
  ;

oilist_clause
  : literal top_element_list_opt
  ;

/*===--------------------------------------------------------------------===*/
/* AttrOrType 专用：params / struct(params)                              */
/*===--------------------------------------------------------------------===*/

params_directive
  : KW_PARAMS
  ;

struct_directive
  : KW_STRUCT L_PAREN struct_arg_list_opt R_PAREN
  ;

struct_arg_list_opt
  : %empty
  | struct_arg_list
  ;

struct_arg_list
  : struct_arg
  | struct_arg_list COMMA struct_arg
  ;

struct_arg
  : variable
  | params_directive
  | custom_directive
  ;

/*===--------------------------------------------------------------------===*/
/* 可选组：( then-elems ^ [ : ( else-elems ) ] ) ?                       */
/* 锚点 ^ 紧跟某个元素；then 里没有锚点则 else 里的锚点使组取反。           */
/*===--------------------------------------------------------------------===*/

optional_group
  : L_PAREN group_then opt_else_group R_PAREN QUESTION
  ;

group_then
  : group_element_list
  ;

opt_else_group
  : %empty
  | COLON L_PAREN group_else R_PAREN
  ;

group_else
  : group_element_list
  ;

group_element_list
  : %empty
  | group_element_list group_element
  ;

group_element
  : top_element
  | top_element CARET
  ;

%%
