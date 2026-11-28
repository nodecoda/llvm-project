/*===-- arith-grammar.y - the concrete syntax of one dialect ----*- yacc -*-===//
 *
 * 这一份文法只描述 arith 方言的**文本语句**，不描述方言定义、不认识 TableGen、
 * 不认识别的方言。输入里除了本方言的语句，其余一律当注释跳过。
 *
 * 模型（详见 26 章）：
 *   - 一个 op 是一类**语句**，助记符是这类语句的**引导词**（像 if/while/print）；
 *   - 56 个助记符落成 8 类语句，形状相同的 op 共用一个产生式；
 *   - 语句的结束由文法决定（`:` type / `to` type 是语句的一部分），不由换行
 *     或括号平衡决定——反例见 mlir/test/Dialect/Arith/canonicalize.mlir:5606
 *     （`arith.subui_extended` 的类型列表在下一行，行本身是括号平衡的）；
 *   - 结果列表只是语句前缀，结果**个数**由语句类决定，无法从类型列表推出：
 *     `mulsi_extended %a, %b : i32` 是 2 结果 1 类型，
 *     `addui_extended %a, %b : i32, i1` 是 2 结果 2 类型。
 *
 * 五个"外来槽位"（本方言不解析内部，但形状受约束）：
 *   TYPE      builtin 类型（`i32`、`vector<8xi64>`、`tensor<?xi32>`）
 *   VALUE     常量的字面量/typed attribute（`42`、`true`、`dense<[...]>`）
 *   ATTRDICT  `{ ... }`，配平
 *   FLAGS     `fastmath<...>` / `overflow<...>` 的旗标表，词法器校验旗标名
 *   LOC       `loc(...)`，配平
 *
 * 不在本方言语句内：generic 形式 `"arith.addi"(%a, %b) : (i32, i32) -> i32`。
 * 那是 IR 语言给所有 op 的通用语句，不是方言的语句类（语料里 18 条）。
 *
 * 验证：bison -Wall -Wcounterexamples -Werror arith-grammar.y
 *===----------------------------------------------------------------------===*/

%start unit

%token RESULTS
%token OP_CONST OP_UNARY OP_BINARY OP_CMP OP_SELECT OP_CAST OP_MULTI OP_SCALING
%token SSA TYPE VALUE BOOL ATTRDICT FLAGS LOC PRED STRING ROUND OTHER
%token COLON COMMA EQUAL KW_TO KW_FASTMATH KW_OVERFLOW KW_EXACT KW_NNEG

%define parse.trace

%code provides {
int yylex(void);
void yyerror(const char *msg);
void arith_note(const char *cls);
}

%%

/*===--------------------------------------------------------------------===*/
/* 单元：一条语句（驱动层在 token 流里找语句起点，语句的结束由这里决定）  */
/*===--------------------------------------------------------------------===*/

unit
  : statement loc_opt       { YYACCEPT; }
  ;

loc_opt
  : %empty
  | LOC
  ;

/* 结果前缀：`%a =` 或 `%a, %b =`。个数是语句类的属性，这里不校验。 */
opt_results
  : %empty
  | RESULTS
  ;

statement
  : opt_results OP_CONST   const_tail    { arith_note("const"); }
  | opt_results OP_UNARY   unary_tail    { arith_note("unary"); }
  | opt_results OP_BINARY  binary_tail   { arith_note("binary"); }
  | opt_results OP_CMP     cmp_tail      { arith_note("cmp"); }
  | opt_results OP_SELECT  select_tail   { arith_note("select"); }
  | opt_results OP_CAST    cast_tail     { arith_note("cast"); }
  | opt_results OP_MULTI   multi_tail    { arith_note("multi"); }
  | opt_results OP_SCALING scaling_tail  { arith_note("scaling"); }
  ;

/*===--------------------------------------------------------------------===*/
/* 8 类语句的尾部                                                        */
/*===--------------------------------------------------------------------===*/

/* 常量：`attr-dict? value`，类型有时缺席（`arith.constant true` 的 i1 在值里）。
 * 值不能是 SSA 名——mlir/test/Dialect/Arith/invalid.mlir:923 就是这条负例
 * （`arith.constant` 后面直接跟 `%x`，上游报 "expected attribute value"）。 */
const_tail
  : attr_dict_opt cvalue opt_type
  ;

/* 常量值：数、dense</dense_resource< 字面量、字符串，或布尔字面量。 */
cvalue
  : VALUE
  | BOOL
  ;

opt_type
  : %empty
  | COLON TYPE
  ;

/* 一元：`operand [fastmath<...>] : type`（negf、flush_denormals）。 */
unary_tail
  : SSA opt_fastmath attr_dict_opt COLON TYPE
  ;

/* 二元：`operand "," operand` 后接四个可选旗标槽位，顺序固定：
 * overflow → exact → roundingmode（裸词）→ fastmath。顺序不能换，因为
 * 上游格式串就是这么写的（ArithOps.td:203 / :215 / :126 / :90）。 */
binary_tail
  : SSA COMMA SSA opt_overflow opt_exact opt_round opt_fastmath
    attr_dict_opt COLON TYPE
  ;

/* 比较：谓词在首位。谓词可以写成裸词（`slt`）或加引号（`"eq"`，
 * 见证 canonicalize.mlir:3120）；缺谓词是错（invalid.mlir:245）。 */
cmp_tail
  : predicate COMMA SSA COMMA SSA opt_fastmath attr_dict_opt COLON TYPE
  ;

/* 谓词三种拼写：裸关键字（slt）、加引号（"eq"）、布尔字面量（true/false 也是
 * CmpFPredicate 的取值，见 canonicalize.mlir:4572）。 */
predicate
  : PRED
  | STRING
  | BOOL
  ;

/* select：条件在最前；类型可以是 1 个（条件为 i1）或 2 个（条件有形状）。 */
select_tail
  : SSA COMMA SSA COMMA SSA attr_dict_opt COLON TYPE opt_type_tail
  ;

opt_type_tail
  : %empty
  | COMMA TYPE
  ;

/* 转型：`operand flags* ":" type "to" type`。`to` 是关键字，不是标识符。 */
cast_tail
  : SSA opt_nneg opt_overflow opt_round opt_fastmath
    attr_dict_opt COLON TYPE KW_TO TYPE
  ;

/* 多结果：类型个数由语句类决定，见文件头。 */
multi_tail
  : SSA COMMA SSA attr_dict_opt COLON TYPE opt_type_tail
  ;

/* scaling：两个操作数、两个输入类型、一个输出类型。 */
scaling_tail
  : SSA COMMA SSA opt_round opt_fastmath
    attr_dict_opt COLON TYPE COMMA TYPE KW_TO TYPE
  ;

/*===--------------------------------------------------------------------===*/
/* 旗标槽位：形状在文法里，旗标名由词法器校验                            */
/*===--------------------------------------------------------------------===*/

opt_fastmath
  : %empty
  | KW_FASTMATH FLAGS
  ;

opt_overflow
  : %empty
  | KW_OVERFLOW FLAGS
  ;

opt_exact
  : %empty
  | KW_EXACT
  ;

opt_nneg
  : %empty
  | KW_NNEG
  ;

opt_round
  : %empty
  | ROUND
  ;

attr_dict_opt
  : %empty
  | ATTRDICT
  ;

%%
