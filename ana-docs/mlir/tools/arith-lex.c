/*===-- arith-lex.c - reference lexer and driver for arith-grammar.y ---*- C -*-===//
 *
 * 参考词法器 + 驱动，只为证明 `arith-grammar.y` 能解析真实 arith 文本。
 *
 * 分工（26 章 §5）：
 *   - 词法器负责把"助记符"翻译成语句类关键字（引导词表 = OP_*），
 *     就像 if/while 在词法层就是不同的关键字；同时负责把五个外来槽位
 *     （TYPE / VALUE / ATTRDICT / FLAGS / LOC）各扫成一个 token。
 *   - 文法负责语句的形状（8 类），语句在哪里结束由文法决定。
 *   - 驱动负责"其余当注释"：在 token 流里找语句起点（RESULTS 或 OP_*），
 *     调用一次 yyparse；解析成功就跳到语句末尾，失败就右移一个 token 重试。
 *
 * 构建与运行：
 *   bison -d -o /tmp/ag.c ana-docs/mlir/arith-grammar.y
 *   cc -I/tmp -o /tmp/arith /tmp/ag.c ana-docs/mlir/tools/arith-lex.c
 *   /tmp/arith mlir/test/Dialect/Arith/ops.mlir
 *===----------------------------------------------------------------------===*/

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include <string.h>

#include "ag.h"

/*===--------------------------------------------------------------------===*/
/* 引导词表：助记符 -> 语句类。56 条，来自 mlir/include/mlir/Dialect/       */
/* Arith/IR/ArithOps.td 的 def 列表；类别由它继承的基类格式决定。           */
/*===--------------------------------------------------------------------===*/

struct mn {
  const char *name;
  int tok;
};

static const struct mn MNEMONICS[] = {
  {"addf", OP_BINARY},
  {"addi", OP_BINARY},
  {"addui_extended", OP_MULTI},
  {"andi", OP_BINARY},
  {"bitcast", OP_CAST},
  {"ceildivsi", OP_BINARY},
  {"ceildivui", OP_BINARY},
  {"cmpf", OP_CMP},
  {"cmpi", OP_CMP},
  {"constant", OP_CONST},
  {"convertf", OP_CAST},
  {"divf", OP_BINARY},
  {"divsi", OP_BINARY},
  {"divui", OP_BINARY},
  {"extf", OP_CAST},
  {"extsi", OP_CAST},
  {"extui", OP_CAST},
  {"floordivsi", OP_BINARY},
  {"flush_denormals", OP_UNARY},
  {"fptosi", OP_CAST},
  {"fptoui", OP_CAST},
  {"index_cast", OP_CAST},
  {"index_castui", OP_CAST},
  {"maximumf", OP_BINARY},
  {"maximumnumf", OP_BINARY},
  {"maxnumf", OP_BINARY},
  {"maxsi", OP_BINARY},
  {"maxui", OP_BINARY},
  {"minimumf", OP_BINARY},
  {"minimumnumf", OP_BINARY},
  {"minnumf", OP_BINARY},
  {"minsi", OP_BINARY},
  {"minui", OP_BINARY},
  {"mulf", OP_BINARY},
  {"muli", OP_BINARY},
  {"mulsi_extended", OP_MULTI},
  {"mului_extended", OP_MULTI},
  {"negf", OP_UNARY},
  {"ori", OP_BINARY},
  {"remf", OP_BINARY},
  {"remsi", OP_BINARY},
  {"remui", OP_BINARY},
  {"scaling_extf", OP_SCALING},
  {"scaling_truncf", OP_SCALING},
  {"select", OP_SELECT},
  {"shli", OP_BINARY},
  {"shrsi", OP_BINARY},
  {"shrui", OP_BINARY},
  {"sitofp", OP_CAST},
  {"subf", OP_BINARY},
  {"subi", OP_BINARY},
  {"subui_extended", OP_MULTI},
  {"truncf", OP_CAST},
  {"trunci", OP_CAST},
  {"uitofp", OP_CAST},
  {"xori", OP_BINARY},
};

#define NUM_MNEMONICS (int)(sizeof(MNEMONICS) / sizeof(MNEMONICS[0]))

/* 舍入模式：5 个裸词（Arith_RoundingModeAttr）。
 * to_nearest_away 是语料抓出来的：第一版漏了它，ops.mlir:804 报错。 */
static const char *ROUND_MODES[] = {"to_nearest_even", "to_nearest_away",
                                    "upward", "downward", "toward_zero"};

/* 谓词（CmpI 的 10 个 + CmpF 的 16 个）。谓词也可以写成加引号的字符串，
 * 见 canonicalize.mlir:3120 的 arith.cmpi "eq"。 */
static const char *PREDICATES[] = {
  "eq", "ne", "slt", "sle", "sgt", "sge", "ult", "ule", "ugt", "uge",
  "oeq", "ogt", "oge", "olt", "ole", "one", "ord",
  "ueq", "ugt", "uge", "ult", "ule", "une", "uno"
};

/* 旗标名：fastmath 与 overflow 共用一张词表，词法器逐个校验。 */
static const char *FLAG_NAMES[] = {
  "none", "fast", "nnan", "ninf", "nsz", "arcp", "contract", "afn", "reassoc",
  "nsw", "nuw"
};

/*===--------------------------------------------------------------------===*/
/* token 流                                                              */
/*===--------------------------------------------------------------------===*/

#define MAXTOK 2000000

static int tok[MAXTOK];
static int tline[MAXTOK];
static char tmn[MAXTOK][40];
static int ntok;
static int prev_tok;

static const char *buf;
static long blen, pos;
static int line = 1;
static int want_flags;
static int nerr;

static int ip;              /* 当前 token 下标 */
static long last_tok;       /* 最近一次返回的 token 下标 */
static int eof_seen;
int arith_last_pos;         /* 语句结束后应从哪里继续 */

static const char *cur_class = "?";
static const char *cur_mn = "?";
static long nstmt;
static long by_class[8];

static int is_op(int t) {
  return t >= OP_CONST && t <= OP_SCALING;
}

static int str_in(const char *s, const char **tab, int n) {
  for (int i = 0; i < n; i++)
    if (!strcmp(s, tab[i])) return 1;
  return 0;
}

/*===--------------------------------------------------------------------===*/
/* 扫描器                                                                */
/*===--------------------------------------------------------------------===*/

static void skip_ws(void) {
  for (;;) {
    while (pos < blen && (buf[pos] == ' ' || buf[pos] == '\t' ||
                          buf[pos] == '\r' || buf[pos] == '\n')) {
      if (buf[pos] == '\n') line++;
      pos++;
    }
    if (pos + 1 < blen && buf[pos] == '/' && buf[pos + 1] == '/') {
      while (pos < blen && buf[pos] != '\n') pos++;
      continue;
    }
    if (pos + 2 < blen && buf[pos] == '{' && buf[pos + 1] == '-' &&
        buf[pos + 2] == '#') {
      pos += 3;
      while (pos + 1 < blen && !(buf[pos] == '#' && buf[pos + 1] == '-')) {
        if (buf[pos] == '\n') line++;
        pos++;
      }
      pos = pos + 2 <= blen ? pos + 2 : blen;
      continue;
    }
    if (pos + 1 < blen && buf[pos] == '/' && buf[pos + 1] == '*') {
      pos += 2;
      while (pos + 1 < blen && !(buf[pos] == '*' && buf[pos + 1] == '/')) {
        if (buf[pos] == '\n') line++;
        pos++;
      }
      pos = pos + 2 <= blen ? pos + 2 : blen;
      continue;
    }
    break;
  }
}

static int read_word(char *out, int cap) {
  int n = 0;
  while (pos < blen && (isalnum((unsigned char)buf[pos]) || buf[pos] == '_')) {
    if (n < cap - 1) out[n++] = buf[pos];
    pos++;
  }
  out[n] = 0;
  return n;
}

/* 从 pos 处扫一个配平的括号区，返回 1 表示配平。 */
static int balanced(int open, int close) {
  int depth = 0;
  while (pos < blen) {
    char c = buf[pos];
    if (c == '"') {
      pos++;
      while (pos < blen && buf[pos] != '"') {
        if (buf[pos] == '\\') pos++;
        if (pos < blen && buf[pos] == '\n') line++;
        pos++;
      }
      if (pos < blen) pos++;
      continue;
    }
    if (c == open) {
      depth++;
      pos++;
      continue;
    }
    if (c == close) {
      if (close == '>' && pos > 0 && buf[pos - 1] == '-') { pos++; continue; }
      depth--;
      pos++;
      if (depth == 0) return 1;
      continue;
    }
    if (c == '\n') line++;
    pos++;
  }
  return 0;
}

/* %name 或 %name#N：成功返回 1 并前进。 */
static int scan_ssa(void) {
  if (pos >= blen || buf[pos] != '%') return 0;
  long p = pos + 1;
  /* SSA 名可以以 '-' 开头：canonicalize.mlir:3650 的 %-inf。 */
  if (p >= blen || !(isalnum((unsigned char)buf[p]) || buf[p] == '_' ||
                     buf[p] == '-' || buf[p] == '.'))
    return 0;
  while (p < blen && (isalnum((unsigned char)buf[p]) || buf[p] == '_' ||
                      buf[p] == '.' || buf[p] == '-' || buf[p] == '$'))
    p++;
  if (p < blen && buf[p] == '#') {          /* %inner#0 */
    long q = p + 1;
    while (q < blen && isdigit((unsigned char)buf[q])) q++;
    if (q > p + 1) p = q;
  }
  pos = p;
  return 1;
}

/* 结果前缀 `%a (, %b)* =` 且后面紧跟 arith.<mn> 才算语句起点；
 * 否则整个前缀退回，按普通 SSA 处理（那就是别的方言的操作数）。 */
static int try_results(void) {
  long save = pos;
  int saveline = line;
  for (;;) {
    if (!scan_ssa()) goto fail;
    skip_ws();
    if (pos < blen && buf[pos] == ',') { pos++; skip_ws(); continue; }
    if (pos < blen && buf[pos] == '=') { pos++; break; }
    goto fail;
  }
  skip_ws();
  if (pos + 6 <= blen && !strncmp(buf + pos, "arith.", 6)) return 1;
fail:
  pos = save;
  line = saveline;
  return 0;
}

static void scan_string(void) {
  pos++;
  while (pos < blen && buf[pos] != '"') {
    if (buf[pos] == '\\') pos++;
    if (pos < blen && buf[pos] == '\n') line++;
    pos++;
  }
  if (pos < blen) pos++;
}

static void scan_number(void) {
  if (pos < blen && buf[pos] == '-') pos++;
  if (pos + 1 < blen && buf[pos] == '0' && (buf[pos + 1] == 'x' || buf[pos + 1] == 'X')) {
    pos += 2;
    while (pos < blen && isxdigit((unsigned char)buf[pos])) pos++;
    return;
  }
  while (pos < blen && isdigit((unsigned char)buf[pos])) pos++;
  if (pos < blen && buf[pos] == '.') {
    pos++;
    while (pos < blen && isdigit((unsigned char)buf[pos])) pos++;
  }
  if (pos < blen && (buf[pos] == 'e' || buf[pos] == 'E')) {
    long q = pos + 1;
    if (q < blen && (buf[q] == '+' || buf[q] == '-')) q++;
    if (q < blen && isdigit((unsigned char)buf[q])) {
      pos = q;
      while (pos < blen && isdigit((unsigned char)buf[pos])) pos++;
    }
  }
}

/* <flags>：词法层校验旗标名，写错就记一次错。 */
static void scan_flags(void) {
  char w[64];
  pos++;                                   /* '<' */
  for (;;) {
    skip_ws();
    if (pos >= blen) return;
    if (buf[pos] == '>') { pos++; return; }
    if (buf[pos] == ',') { pos++; continue; }
    if (isalpha((unsigned char)buf[pos]) || buf[pos] == '_') {
      int n = 0;
      while (pos < blen && (isalnum((unsigned char)buf[pos]) || buf[pos] == '_')) {
        if (n < 63) w[n++] = buf[pos];
        pos++;
      }
      w[n] = 0;
      if (!str_in(w, FLAG_NAMES, (int)(sizeof(FLAG_NAMES) / sizeof(FLAG_NAMES[0])))) {
        fprintf(stderr, "%s:%d: error: unknown flag '%s'\n", "<input>", line, w);
        nerr++;
      }
      continue;
    }
    pos++;
  }
}

static int lookup_mnemonic(const char *w) {
  for (int i = 0; i < NUM_MNEMONICS; i++)
    if (!strcmp(w, MNEMONICS[i].name)) return MNEMONICS[i].tok;
  return OTHER;
}

/* `{...}` 只有出现在"操作数位置"之后才是 attr-dict；函数体的 `{` 是垃圾，
 * 不能当 attr-dict 吞掉整个 region（否则语句全被吃掉）。 */
static int dict_position(int t) {
  return t == SSA || t == COMMA || t == OP_CONST || t == KW_EXACT ||
         t == KW_NNEG || t == FLAGS || t == ROUND;
}

/* `{` 在 SSA 之后有两种含义：attr-dict（`exact {test.marker}`）和别的方言
 * 的 region 体（`scf.for ... %c64 {`）。只看前缀分不开，于是看内容：
 * 属性里不可能出现 SSA 名（`%x`）或块标签（`^bb`），region 体里一定有。
 * 这一条修掉了 12 条被吞掉的语句（int-range-narrowing.mlir:728 起等）：
 * 旧规则把 `scf.for` 的 `{` 当 attr-dict，整个循环体被 balanced() 跳过。 */
static int brace_is_dict(void) {
  long save = pos;
  int saveline = line;
  if (!balanced('{', '}')) {
    pos = save;
    line = saveline;
    return 0;
  }
  long end = pos;
  int dict = 1;
  for (long i = save + 1; i < end - 1;) {
    if (buf[i] == '"') {
      i++;
      while (i < end - 1 && buf[i] != '"') {
        if (buf[i] == '\\') i++;
        i++;
      }
      i++;
      continue;
    }
    if (buf[i] == '%' || buf[i] == '^') {
      dict = 0;
      break;
    }
    i++;
  }
  pos = save;
  line = saveline;
  return dict;
}

static void lex_all(const char *text, long len) {
  buf = text;
  blen = len;
  pos = 0;
  line = 1;
  ntok = 0;
  prev_tok = -1;
  while (1) {
    int wf;
    skip_ws();
    if (pos >= blen) break;
    wf = want_flags;
    want_flags = 0;
    tline[ntok] = line;
    tmn[ntok][0] = 0;
    char c = buf[pos];
    if (c == '%') {
      if (try_results()) {
        tok[ntok] = RESULTS;
      } else if (scan_ssa()) {
        tok[ntok] = SSA;
      } else {
        pos++;                       /* 不能卡住：消耗一个字符 */
        tok[ntok] = OTHER;
      }
    } else if (c == '"') {
      scan_string();
      tok[ntok] = STRING;
    } else if (isdigit((unsigned char)c) ||
               (c == '-' && pos + 1 < blen && isdigit((unsigned char)buf[pos + 1]))) {
      scan_number();
      tok[ntok] = VALUE;
    } else if (isalpha((unsigned char)c) || c == '_') {
      char w[256];
      read_word(w, sizeof w);
      if (!strcmp(w, "arith") && pos < blen && buf[pos] == '.') {
        char mn[40];
        pos++;
        read_word(mn, sizeof mn);
        int t = lookup_mnemonic(mn);
        tok[ntok] = t;
        if (t != OTHER) snprintf(tmn[ntok], sizeof tmn[ntok], "%s", mn);
      } else if ((!strcmp(w, "dense") || !strcmp(w, "dense_resource")) &&
                 pos < blen && buf[pos] == '<') {
        balanced('<', '>');
        tok[ntok] = VALUE;
      } else if (!strcmp(w, "loc") && pos < blen && buf[pos] == '(') {
        balanced('(', ')');
        tok[ntok] = LOC;
      } else if (!strcmp(w, "fastmath")) {
        want_flags = 1;
        tok[ntok] = KW_FASTMATH;
      } else if (!strcmp(w, "overflow")) {
        want_flags = 1;
        tok[ntok] = KW_OVERFLOW;
      } else if (!strcmp(w, "exact")) {
        tok[ntok] = KW_EXACT;
      } else if (!strcmp(w, "nneg")) {
        tok[ntok] = KW_NNEG;
      } else if (!strcmp(w, "to")) {
        tok[ntok] = KW_TO;
      } else if (!strcmp(w, "true") || !strcmp(w, "false")) {
        tok[ntok] = BOOL;
      } else if (str_in(w, PREDICATES, (int)(sizeof(PREDICATES) / sizeof(PREDICATES[0])))) {
        tok[ntok] = PRED;
      } else if (str_in(w, ROUND_MODES, (int)(sizeof(ROUND_MODES) / sizeof(ROUND_MODES[0])))) {
        tok[ntok] = ROUND;
      } else {
        if (pos < blen && buf[pos] == '<') balanced('<', '>');   /* vector<...> */
        tok[ntok] = TYPE;
      }
    } else if (c == '(') {
      /* `({ ... })` 是别的方言的 region（`"test.foo"(%a) ({ ^bb0: ... })`），
       * 不是函数类型：只消耗 `(`，让花括号里的 arith 语句照常被切出来。
       * 是这一条修掉了 int-range-opts-crash.mlir:9 之类被整段吞掉的语句。 */
      long q = pos + 1;
      while (q < blen && (buf[q] == ' ' || buf[q] == '\t' || buf[q] == '\n'))
        q++;
      if (q < blen && buf[q] == '{') {
        pos++;
        tok[ntok] = OTHER;
      } else {
        /* 函数类型：`() -> ()`、`(i32, i32) -> i32`（invalid.mlir:294 见证）。 */
        balanced('(', ')');
        skip_ws();
        if (pos + 1 < blen && buf[pos] == '-' && buf[pos + 1] == '>') {
          pos += 2;
          skip_ws();
          if (pos < blen && buf[pos] == '(') balanced('(', ')');
        }
        tok[ntok] = TYPE;
      }
    } else if (c == '<' && wf) {
      scan_flags();
      tok[ntok] = FLAGS;
    } else if (c == '{' && dict_position(prev_tok) && brace_is_dict()) {
      balanced('{', '}');
      tok[ntok] = ATTRDICT;
    } else if (c == ':') {
      pos++; tok[ntok] = COLON;
    } else if (c == ',') {
      pos++; tok[ntok] = COMMA;
    } else if (c == '=') {
      pos++; tok[ntok] = EQUAL;
    } else {
      pos++; tok[ntok] = OTHER;
    }
    prev_tok = tok[ntok];
    if (getenv("ARITH_TOK"))
      fprintf(stderr, "tok %d line %d cls %d '%s'\n", ntok, tline[ntok],
              tok[ntok], tmn[ntok]);
    ntok++;
    if (ntok >= MAXTOK - 1) break;
  }
}

/*===--------------------------------------------------------------------===*/
/* 与文法对接                                                            */
/*===--------------------------------------------------------------------===*/

void arith_note(const char *cls) {
  cur_class = cls;
  nstmt++;
  if (!strcmp(cls, "const")) by_class[0]++;
  else if (!strcmp(cls, "unary")) by_class[1]++;
  else if (!strcmp(cls, "binary")) by_class[2]++;
  else if (!strcmp(cls, "cmp")) by_class[3]++;
  else if (!strcmp(cls, "select")) by_class[4]++;
  else if (!strcmp(cls, "cast")) by_class[5]++;
  else if (!strcmp(cls, "multi")) by_class[6]++;
  else if (!strcmp(cls, "scaling")) by_class[7]++;
}

static jmp_buf parse_jmp;

void yyerror(const char *msg) {
  fprintf(stderr, "%s:%d: error: %s\n", "<input>", ip > 0 ? tline[ip - 1] : 0, msg);
  nerr++;
  longjmp(parse_jmp, 1);   /* 不做错误恢复：恢复会吞掉后面整段合法语句。 */
}

/* setjmp 放在独立函数里：不要让它和 main 的活跃变量同栈帧，
 * 否则编译器会警告变量可能被 longjmp 破坏。 */
static int parse_one(void) {
  if (setjmp(parse_jmp) == 0) return yyparse();
  return 1;
}

int yylex(void) {
  if (ip >= ntok) {
    eof_seen = 1;
    return 0;
  }
  last_tok = ip;
  if (is_op(tok[ip])) cur_mn = tmn[ip];
  return tok[ip++];
}

/*===--------------------------------------------------------------------===*/
/* 驱动                                                                  */
/*===--------------------------------------------------------------------===*/

int main(int argc, char **argv) {
  int trace = 0, i = 1;
  if (i < argc && !strcmp(argv[i], "--trace")) { trace = 1; i++; }
  if (i >= argc) {
    fprintf(stderr, "usage: %s [--trace] file.mlir...\n", argv[0]);
    return 2;
  }
  int rc = 0;
  for (; i < argc; i++) {
    FILE *f = fopen(argv[i], "rb");
    if (!f) { perror(argv[i]); return 2; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = malloc((size_t)len + 1);
    if (!text) { fclose(f); return 2; }
    if (fread(text, 1, (size_t)len, f) != (size_t)len) { fclose(f); free(text); return 2; }
    text[len] = 0;
    fclose(f);

    nstmt = 0;
    nerr = 0;
    memset(by_class, 0, sizeof by_class);
    ntok = 0;
    line = 1;
    pos = 0;
    want_flags = 0;
    lex_all(text, len);

    long k = 0;
    while (k < ntok) {
      if (tok[k] == RESULTS || is_op(tok[k])) {
        int startline = tline[k];
        ip = (int)k;
        eof_seen = 0;
        last_tok = k;
        int r = parse_one();
        long next = eof_seen ? ntok : last_tok;
        if (r == 0 && next > k) {
          if (trace)
            fprintf(stdout, "stmt %d: %s arith.%s\n", startline, cur_class, cur_mn);
          k = next;
        } else {
          k = last_tok >= k ? last_tok + 1 : k + 1;
        }
      } else {
        k++;
      }
    }
    fprintf(stdout, "%s: %ld statements, %d errors\n", argv[i], nstmt, nerr);
    if (nerr) rc = 1;
    free(text);
  }
  return rc;
}
