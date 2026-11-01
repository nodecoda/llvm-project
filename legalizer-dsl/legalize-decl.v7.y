/*
 * 2026-11-01  Legalizer declaration language  (v7: 形状与每槽策略)
 *
 * 本文件是 v6 的**加法**：v6 的每一条 production、每一个 token 的含义都不变，
 * 只追加三样东西。用 v6 写的 .decl 在 v7 下逐字节同义。
 *
 * v8 追加第四样，同样是加法：`env { }` 与 `table over`。v7 的读取器仍然
 * 接受不写这两个的文件，并保持 v7 的语义（原子从文本里猜）。写了 `env { }`
 * 之后，原子就是声明的：`mode` / `feature` 是采样维度，`let` 是**定义**，
 * 在枚举之前被替换掉，所以永远不是维度。详见文件末尾的 envdef。
 * 没有 `assume`：一个能关掉的默认值（x86-64 的 SSE2）不是不变式。
 *
 * 追加的三样，全部来自测量（见 2026-11-01-default-inference.md §5.6 与
 * 2026-11-01-x86.decl 的 84 条 prefer）：
 *
 *  1. query ... ;        形状是声明的，不是"谁提过 ty(i)"推出来的
 *     - 现在 arity 有两个来源：chain_arity 读链、slot_count 读 .decl，
 *       两者都不是指令的属性。删掉一条 slot-1 规则，查询空间会静默缩小
 *       （chain_arity 的 docstring 自己承认了）。指令有几个类型变量在
 *       GenericOpcodes.td 里是机器可读的（type0/type1/ptype0），这里把它
 *       变成声明，让 lint 能对两个方向都报错（提到未声明 = error，
 *       声明未提到 = "slot i unconstrained" warning）。
 *     - `variadic i;` 说明变参操作数共享第 i 个类型变量（对应
 *       `variadicOpsType`）。G_MERGE_VALUES / G_UNMERGE_VALUES /
 *       G_BUILD_VECTOR / G_CONCAT_VECTORS 的 2 槽模型只有在这条成立时才对。
 *
 *  2. policy i: ... ;    每槽的默认，界可以是**名义的**
 *     - 今天 `class int;` 只能作用于槽 0、只能从合法集取凸包：
 *       promote 到最小合法标量、truncate 到最大合法标量。
 *     - 84 条 prefer 里 55 条不是这个形状：它们是"不管合法集怎么写，
 *       就往 T 走"。例子：
 *         G_FCMP   clampScalar(0, s8, s8)          → 槽 0 全压到 s8
 *         G_ANYEXT clampScalar(1, s8, sMaxScalar)  → 槽 1 无合法投影，只能给常数
 *         G_SELECT clampScalar(0, ..., sMaxScalar) → 上界 64，而凸包上界是 80
 *     - `hull` 保留今天的语义（含 fail-closed：投影为空或不是区间 → 报错，
 *       而不是静默不动——UNMERGE 的 144 点和 LOAD 的 50 点就是静默不动的产物）。
 *     - `class int;` 保留为 `policy 0: scalar hull;` 的别名，现有 13 个块零改动。
 *
 *  3. imm(i)             查询的第三个坐标
 *     - LegalityQuery 是 (Opcode, Types[], MMODescs[], Immediates[])；
 *       LegalityPredicates 有 immediateIs / immediateInSet / immediateIsNot。
 *       目前**没有任何 target 用它们**（全 lib/ grep 为 0），X86 的
 *       G_INSERT/G_EXTRACT 用的是 legalForTypeWithAnyImm。
 *     - 所以这一版只占位、不建推断：文法接受 imm(i)，语义要求"用了就得声明"，
 *       而"opcode 有 imm 坐标、块里一次没提"给出 warning `immediates unchecked`。
 *       今天 G_INSERT 的 offset 就是这样被静默忽略的：语义上合法（AnyImm），
 *       但声明里应该说出来，而不是看起来像漏了。
 *
 * 词法约定（同 v6）：
 *   TYPESPEC := s<N> | p<A>:<N> | v<L>s<N> | v<L>p<A>:<N>
 *   OPCODE   := G_[A-Z0-9_]+
 *   IDENT    := [A-Za-z_][A-Za-z0-9_]*   其余
 *
 * 验证：bison 3.8.2 -Wall → 0 error / 0 warning / 0 conflict。
 *
 * ---------------------------------------------------------------------------
 * 文法一页纸（EBNF）。分隔符一人一职：`[ ]` 是有序列表，`{ }` 是块/作用域，
 * `( )` 只用于表达式分组和产品（product）。
 *
 *   file      = { macrodef | classdef | envdef | tabledef | opdef } ;
 *   envdef    = "env" "{" { envdecl } "}" ;
 *   envdecl   = ( "mode" | "feature" ) ident_list [ "from" IDENT ] ";"
 *             | "let" IDENT "=" expr ";" ;
 *   tabledef  = "table" "over" ident_list ";" ;
 *   opdef     = "op" opcode { "," opcode } block ;
 *   block     = "{" { item } "}" ;
 *   item      = query | variadic | groupdef | with | legal | prefer
 *             | policy | follow | except | shapes | elemwise | class_use ;
 *
 *   list      = "[" [ elem { "," elem } ] "]" | elem { "," elem } ;
 *   query     = "query" list ";" ;        (* 元数 = 声明的坐标个数 *)
 *   groupdef  = "group" IDENT "=" list ";" ;
 *   with      = "with" ( IDENT | list ) block ;   (* 块 = 槽位作用域 *)
 *   policy    = "policy" [ INT ] ":" kind bound [ "pow2" [ INT ] ] [ "when" expr ] ";" ;
 *   bound     = "to" expr | "promote" expr | "truncate" expr
 *             | "[" bound "," bound "]"        (* 恰好两个：下界、上界，位置固定 *)
 *             | "hull" ;
 *
 * 列表是有序的、位置有意义的：`[s8, s16]` 是点，`[a, b], [c, d]` 才是并。
 * 这不是集合语法；集合仍然由 `,`（并）和 `+`（并）在 `expr` 里表达。
 *
 * ---------------------------------------------------------------------------
 * 规则优先级与覆盖性质（这两条是语义，文法只负责能把它们写出来）：
 *
 *   priority   specific > default，tie = error       （同 yacc 的 %nonassoc）
 *   coverage   slot     每个声明的 `ty` 坐标都被约束   -> 13-slot-unconstrained
 *              default  特指 ∪ 默认 = 声明的全部槽      -> 13-policy-duplicate
 *              answer   policy* 落在 legal ∪ escape      -> 13-policy-unreachable
 *                       且不得在 block 无答案的配置里承诺 -> 13-policy-dead-config
 *
 * 覆盖是**可检查的属性**，不是注释：三条各有一个检查点，都在 check_decl 里。
 * 第三条只对 `policy` 生效，并且按 realizable 的 feature assignment 逐个算。
 */

%{
#include <string>
/* Provided by the driver (tools/parse_check.cc); a grammar on its own does
 * not own a lexer, and the C skeleton needs the declarations in scope. */
int yylex();
void yyerror(const char *);
%}
%union { int ival; std::string *sval; }

%token OP MACRO LEGAL PREFER FOLLOW EXCEPT WHEN ON FROM BY
%token SET SPLIT DELEGATE UNSUPPORTED
%token TY MEM OPC KIND BITS ELEM LANES ADDRSPACE SIZE
%token CLASS ELEMWISE SHAPES ANY NEVER COND POW2 MIN
%token SCALAR VECTOR POINTER PTRVECTOR TRUE FALSE
%token PLUS MINUS STAR SLASH IN
%token EQ NE LT LE GT GE ANDAND OROR BANG
%token LBRACE RBRACE LPAREN RPAREN COMMA SEMI COLON DOT UNDERSCORE ARROW
%token <sval> IDENT TYPESPEC OPCODE
%token <ival> INT

/* --- v7 additions ------------------------------------------------------ */
%token QUERY VARIADIC POLICY IMM TO HULL LBRACKET RBRACKET
%token WITH GROUP
%token PROMOTE TRUNCATE

/* --- v8 additions: the declared environment ------------------------- */
%token ENV MODE FEATURE LET TABLE OVER

%left OROR
%left ANDAND
%nonassoc EQ NE LT LE GT GE IN
%precedence BANG
%left PLUS MINUS
%left STAR SLASH

%start file
%%
file      : %empty | file defn ;
defn      : opdef | macrodef | classdef | envdef | tabledef ;

/* v8: the alphabet is declared, not guessed.
 *
 * v7 discovered the construction-time features by scanning the block text for
 * identifiers that are not types, macros, classes or query predicates.  That
 * guess cannot tell three different things apart, and the measurement showed
 * what it costs:
 *
 *   mode     Is64Bit                     comes from the triple, is not a feature
 *            bit, and changes the *type universe* (p0:64 vs p0:32) and the
 *            macros, not just a guard.
 *   feature  HasSSE2, HasAVX512, ...     comes from -mcpu/-mattr, per function.
 *   let      HasCMOV = cmov || HasSSE1 || Is64Bit
 *                                        a *definition*: it is substituted
 *                                        away before enumeration, so the
 *                                        declared name is never a dimension.
 *
 * The distinction is not cosmetic.  Treating a definition as a dimension is
 * how `{HasCMOV=0, Is64Bit=1}` -- impossible, because canUseCMOV() is
 * `hasCMOV() || hasSSE1() || is64Bit()` -- got enumerated as reachable and
 * diluted a measurement by 6 of 301 environments.
 *
 * What is deliberately NOT here: an `assume` clause.  `llc -mtriple=x86_64
 * -mattr=-sse2` exits 0, so "x86-64 implies SSE2" is a default, not a fact;
 * writing it as an assumption would bake a switchable setting into the spec.
 * Which configurations a generated table covers is a `table over` clause --
 * a commitment, checked at the point of use -- not a truth about the target.
 */
envdef    : ENV LBRACE envbody RBRACE ;

envbody   : %empty | envbody envitem ;
envitem   : envdecl | letdef ;
envdecl   : MODE ident_list from_clause SEMI
          | FEATURE ident_list from_clause SEMI ;
from_clause : %empty | FROM IDENT ;
letdef    : LET IDENT EQ expr SEMI ;

/* `table over Is64Bit;`  -- the environments a generated table is built for.
 * The generator must emit the matching gate (`if (!Is64Bit) -> Outside`); a
 * gate narrower than this clause answers wrongly, a wider one silently loses
 * the fallback. */
tabledef  : TABLE OVER ident_list SEMI ;
ident_list: IDENT | ident_list COMMA IDENT
          | LBRACKET ident_list RBRACKET ;

opdef     : OP opcodes LBRACE body RBRACE ;   /* query/variadic are items */

/* v7: the shape of the query is a property of the instruction, not of which
 * `ty(i)` someone happened to write.  `query ty 0, ty 1, mem 0, imm 0;`
 * declares the coordinates; lint checks both directions.  Absent = the block
 * is a whole-op terminal (`legal any;` / `except true -> ...`) or an old v6
 * block, which keeps v6 semantics: arity is inferred as before. */
query     : QUERY coords SEMI ;
coords    : coord | coords COMMA coord
          | LBRACKET coord_list RBRACKET ;
coord_list: coord | coord_list COMMA coord ;
coord     : TY INT | MEM INT | IMM INT ;

/* v7: variadic operands after the declared ones share this type variable.
 * `variadic 1;` is what `variadicOpsType = type1` means in the opcode record. */
variadic  : VARIADIC INT SEMI ;

/* v7: slot groups.  A group is a *slot set*, not a rule set: it changes what
 * an omitted slot means inside its braces, nothing else.
 *
 *   group elem = 0, 1;
 *   with elem {
 *     policy: scalar hull;          -- policy 0 and policy 1
 *     prefer s32 on 1;              -- explicit: wins over the general one
 *   }
 *
 * `with` bodies nest; an inner set must be a subset of the outer one (checked
 * in check_decl, `13-with-nested`).  Only slot-scoped statements may appear
 * inside (`policy`, `prefer`, `shapes`); a `legal` clause is positional and
 * has no slot to group, so it is an error there (`13-with-non-slot`). */
groupdef  : GROUP IDENT EQ slot_list SEMI ;
with_decl : WITH slot_set LBRACE body RBRACE ;
slot_set  : slot_list | IDENT ;
slot_list : INT | slot_list COMMA INT
          | LBRACKET int_list RBRACKET ;
int_list  : INT | int_list COMMA INT ;

/* A type class is the one thing LLT cannot tell us: `s32` is i32 or float.
 * The membership is a set of concrete types, and `class int;` inside a block
 * means "apply the width policy to slot 0 types in this class". */
classdef  : CLASS IDENT EQ exprs SEMI ;
opcodes   : OPCODE | opcodes COMMA OPCODE ;

macrodef  : MACRO IDENT EQ expr SEMI
          | MACRO IDENT LPAREN params RPAREN EQ expr SEMI ;
params    : %empty | paramlist ;
paramlist : param | paramlist COMMA param ;
param     : IDENT COLON IDENT ;

body      : %empty | body item ;
item      : legal_decl | prefer_decl | follow_decl | except_decl
          | class_decl | elemwise_decl | shapes_decl
          | query | variadic | policy_decl
          | groupdef | with_decl
          | expr SEMI ;   /* macro_use: macro applying a block */

/* --- FACT ------------------------------------------------------------ */
legal_decl : LEGAL exprs SEMI
           | LEGAL exprs WHEN expr SEMI ;
exprs      : expr | exprs COMMA expr ;          /* 逗号 = 并 */

/* --- PREFERENCE ------------------------------------------------------ */
/* A statement without a slot is the **default rule**; a statement with one
 * is a specific rule, and a specific rule beats the default on its slot.
 * The shape is borrowed from the tools this grammar sits next to: `with` is
 * a lex start condition (a scope, not a macro), a `group` is a named
 * definition, and two declarations competing for one slot are `%nonassoc`
 * (an error), not "the last one wins".
 *
 *   prefer s8;               -- default: the slots nothing specific claims
 *   prefer s32 on 1;         -- specific: slot 1, and it wins over the default
 *
 * The default never lands on a slot that has a specific declaration of the
 * same kind (`target` / `pow2` / `elem`): different kinds are different
 * rewrites, so `prefer pow2;` and `prefer s32 on 1;` both stay live on slot 1.
 * Textual order does not matter -- specificity does. */
prefer_decl: PREFER preference on_clause SEMI
           | PREFER preference on_clause WHEN expr SEMI ;
on_clause  : %empty | ON INT ;

/* `pow2` 可以带 MinSize：.td 的 WidenScalar*ToNextPow2<0,32> 是
 * max(nextPow2(elt), 32)，把 32 丢掉就只能靠一条 `&& !pow2` 的伴生规则
 * 去补，而在 s80 上补错（该 s128，补成 s64）。
 * `elem` 是 Scalarize：向量取元素。它不是「推断」，是一条规则。 */
preference : expr
           | POW2 MIN INT
           | ELEM ;

/* --- DEFAULT --------------------------------------------------------- */
/* v7.2: the policy for one slot.
 *
 *   policy: scalar hull;                       -- default rule: every ty coordinate
 *   policy 0: scalar to s8;                    -- both ends s8: clamp everything
 *   policy 1: scalar [s8, MaxScalar] pow2;     -- nominal interval + pow2 floor
 *   policy 0: scalar promote s32 pow2 16;      -- one-sided: no upper bound
 *   policy 0: scalar hull pow2 32;             -- == class int pow2 32
 *   policy 0: vector hull;                     -- == shapes on 0
 *   policy 1: scalar [s8, s64] when HasAVX512;
 *
 * `hull` reads the slot's projection of the legal relation.  If that
 * projection is empty it is an **error** -- v6's behaviour was to silently
 * decline, and a silent decline is indistinguishable from "this slot has no
 * default".  Gaps are allowed and deliberate: G_FCMP's legal slot 0 is
 * {s8,s32,s64,s80}, and the hull's endpoints are still legal by construction.
 *
 * `when` is evaluated once per feature assignment (like a legal guard), not
 * per query: a policy is a property of the target configuration.
 *
 * A `policy` without a slot is the default and fills only the slots that no
 * `policy i:` (or `with`-scoped) policy covers.  `policy: A` plus
 * `policy 1: B` gives slot 0 = A and slot 1 = B, whatever the order.
 */
policy_decl: POLICY on_slot COLON pkind pbound pmods pguard SEMI ;
on_slot   : %empty | INT ;

pkind      : SCALAR | VECTOR ;
pbound     : TO expr                       /* both ends */
           | PROMOTE expr                  /* lower bound only */
           | TRUNCATE expr                 /* upper bound only */
           | LBRACKET pend COMMA pend RBRACKET
           | HULL ;                        /* read both off the legal projection */
/* `hull` is a *value* on either end, not a mode: G_SELECT takes its lower
 * bound from the legal set and its upper bound from a constant.  It is not in
 * `expr` because it is only meaningful where a bound is expected. */
pend       : HULL | expr ;
pmods      : %empty | POW2 | POW2 INT ;
pguard     : %empty | WHEN expr ;

/* `class int;` is kept as the spelling of `policy 0: scalar hull;`.
 * `class int pow2 [N]` == `policy 0: scalar hull pow2 [N];`. */
class_decl : CLASS IDENT SEMI
           | CLASS IDENT POW2 SEMI
           | CLASS IDENT POW2 INT SEMI ;

/* --- VECTOR DEFAULT ------------------------------------------------- */
/* `elemwise;` states a property of the op, not a rewrite: the operation is
 * applied per element, so replacing a vector the rules cannot legalize by its
 * element does not change what the program computes.  That property is the
 * licence the default policy needs -- without it a vector default cannot tell
 * G_ADD (chain: Scalarize) from G_CONSTANT (chain: Unsupported), because both
 * have a legal set with no vector in it.  One word replaces `prefer elem on 0`
 * for every element-wise op, and it is where the lane clamp will hang once
 * feature reachability is declared. */
elemwise_decl : ELEMWISE SEMI ;

/* --- LANE LATTICE ---------------------------------------------------- */
/* `shapes on i;` is the vector half of `class int;`: the slot is on a lane
 * lattice, and the hull -- the narrowest and widest admissible shape for each
 * element type -- is read off the block's own admissible shapes (its `legal`
 * clauses and the shapes its `custom` lowering handles).  It replaces the
 * 2-8 `prefer v<lo><e> / prefer v<hi><e>` pairs the generator used to write.
 * v7: this is `policy i: vector hull;`; the old spelling is kept. */
shapes_decl : SHAPES on_clause SEMI ;

/* --- COUPLING -------------------------------------------------------- */
follow_decl: FOLLOW INT FROM INT BY ELEM SEMI ;

/* --- ESCAPE ---------------------------------------------------------- */
except_decl: EXCEPT expr ARROW action SEMI ;
action     : LEGAL
           | SET LPAREN expr COMMA expr RPAREN
           | SPLIT LPAREN expr COMMA expr RPAREN
           | DELEGATE LPAREN IDENT RPAREN
           | UNSUPPORTED ;

/* --- the single expression grammar ----------------------------------- */
expr : expr OROR expr | expr ANDAND expr | BANG expr
     | expr EQ expr | expr NE expr | expr IN expr
     | expr LT expr | expr LE expr | expr GT expr | expr GE expr
     | expr PLUS expr | expr MINUS expr
     | expr STAR expr | expr SLASH expr
     | LPAREN expr RPAREN
     | LPAREN tuple RPAREN
     | INT | TYPESPEC | IDENT | TRUE | FALSE
     | UNDERSCORE | ANY | NEVER | POW2
     | SCALAR | VECTOR | POINTER | PTRVECTOR
     | TY LPAREN expr RPAREN
     | BITS LPAREN expr RPAREN
     | ELEM LPAREN expr RPAREN
     | LANES LPAREN expr RPAREN
     | KIND LPAREN expr RPAREN
     | ADDRSPACE LPAREN expr RPAREN
     | SIZE LPAREN expr RPAREN
     | MEM LPAREN expr RPAREN
     | MEM LPAREN expr COMMA expr RPAREN
     | MEM LPAREN expr RPAREN DOT TY
     | MEM LPAREN expr RPAREN DOT IDENT
     | IMM LPAREN expr RPAREN          /* v7: the third coordinate */
     | OPC
     | COND LPAREN expr COMMA expr COMMA expr RPAREN
     | IDENT LPAREN arglist RPAREN ;
/* `mem(i).size`, `mem(i).align`, `mem(i).order` all reach the generic
 * `. IDENT` production above; the field names are checked semantically, not
 * grammatically, so a typo is an error in check_decl, not a parse failure. */

tuple : expr COMMA exprs ;
arglist : %empty | args ;
args  : expr | args COMMA expr ;
%%
