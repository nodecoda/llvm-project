/*===-- dsl-lex.c - reference lexer for dialect-dsl.y ---------*- C -*-===//
 *
 * 手工写的参考词法器，只为证明 dialect-dsl.y 能被真实解析，不是产品实现。
 * 三个词法模式：
 *   normal —— 默认模式；遇到 `code {` 进入平衡花括号扫描，整段返回 CODE。
 *   format —— `Format {` 之后；`[ ] < > ,` 是结构符，"..." 是字面量，
 *             其余标识符按 format 指令或变量引用分派。
 *   code   —— 由 scan_code() 内联处理，不单独占用 mode。
 *
 * 构建与运行：
 *   bison -d -o /tmp/dsl.c ana-docs/mlir/design-first-v1/dialect-dsl.y
 *   cc -I/tmp -o /tmp/dsl /tmp/dsl.c ana-docs/mlir/design-first-v1/dsl-lex.c
 *   /tmp/dsl ana-docs/mlir/design-first-v1/examples/arith.mdl
 *
 * 约束：`code` 是普通模式下的保留字；format 模式里 type/operands/results
 * 等指令字也是保留字，变量名不能与它们重名。
 *===----------------------------------------------------------------------===*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "dsl.h"

static FILE *src;
static int line = 1;
static int mode = 0;        /* 0 normal, 1 format */
static int fmt_depth = 0;
static int expect_format = 0;
static char ident[512];

void yyerror(const char *s) { fprintf(stderr, "line %d: %s\n", line, s); }

static int getch(void) { int c = fgetc(src); if (c == '\n') line++; return c; }
static void ungetch(int c) { if (c != EOF) { if (c == '\n') line--; ungetc(c, src); } }

static int read_ident(int first, int allow_dash) {
  int n = 0, c;
  ident[n++] = (char)first;
  while ((c = getch()) != EOF) {
    if (isalnum((unsigned char)c) || c == '_' ||
        (allow_dash && c == '-')) {
      if (n < 511) ident[n++] = (char)c;
    } else { ungetch(c); break; }
  }
  ident[n] = 0;
  return n;
}

static int read_string(void) {
  int c;
  while ((c = getch()) != EOF) {
    if (c == '\\') { (void)getch(); continue; }
    if (c == '"') break;
  }
  return STRING;
}

static int scan_code(void) {
  int depth = 1, c;
  while ((c = getch()) != EOF) {
    if (c == '"') {
      int d;
      while ((d = getch()) != EOF) {
        if (d == '\\') { (void)getch(); continue; }
        if (d == '"') break;
      }
      continue;
    }
    if (c == '/') {
      int d = getch();
      if (d == '/') { while ((d = getch()) != EOF && d != '\n') {} continue; }
      if (d == '*') { int p = 0; while ((d = getch()) != EOF) { if (p == '*' && d == '/') break; p = d; } continue; }
      ungetch(d);
    }
    if (c == '{') depth++;
    else if (c == '}') { if (--depth == 0) break; }
  }
  return CODE;
}

static int normal_kw(const char *s) {
  /* 小写：声明与类型关键字 */
  if (!strcmp(s, "dialect")) return KW_DIALECT;
  if (!strcmp(s, "op")) return KW_OP;
  if (!strcmp(s, "family")) return KW_FAMILY;
  if (!strcmp(s, "attr_def")) return KW_ATTR_DEF;
  if (!strcmp(s, "type_def")) return KW_TYPE_DEF;
  if (!strcmp(s, "interface")) return KW_INTERFACE_DEF;
  if (!strcmp(s, "enum_def")) return KW_ENUM_DEF;
  if (!strcmp(s, "constraint")) return KW_CONSTRAINT;
  if (!strcmp(s, "extends")) return KW_EXTENDS;
  if (!strcmp(s, "optional")) return KW_OPTIONAL;
  if (!strcmp(s, "variadic")) return KW_VARIADIC;
  if (!strcmp(s, "variadic_of_variadic")) return KW_VARIADIC_OF_VARIADIC;
  if (!strcmp(s, "unit")) return KW_UNIT;
  if (!strcmp(s, "true")) return KW_TRUE;
  if (!strcmp(s, "false")) return KW_FALSE;
  /* 首字母大写：子句关键字 */
  if (!strcmp(s, "Operands")) return KW_OPERANDS;
  if (!strcmp(s, "Results")) return KW_RESULTS;
  if (!strcmp(s, "Regions")) return KW_REGIONS;
  if (!strcmp(s, "Successors")) return KW_SUCCESSORS;
  if (!strcmp(s, "Attrs")) return KW_ATTRS;
  if (!strcmp(s, "Props")) return KW_PROPS;
  if (!strcmp(s, "Traits")) return KW_TRAITS;
  if (!strcmp(s, "Implements")) return KW_IMPLEMENTS;
  if (!strcmp(s, "Format")) return KW_FORMAT;
  if (!strcmp(s, "Custom")) return KW_CUSTOM;
  if (!strcmp(s, "Builders")) return KW_BUILDERS;
  if (!strcmp(s, "Build")) return KW_BUILD;
  if (!strcmp(s, "Verify")) return KW_VERIFY;
  if (!strcmp(s, "Fold")) return KW_FOLD;
  if (!strcmp(s, "Canonicalize")) return KW_CANONICALIZE;
  if (!strcmp(s, "Summary")) return KW_SUMMARY;
  if (!strcmp(s, "Extra")) return KW_EXTRA;
  if (!strcmp(s, "Mnemonic")) return KW_MNEMONIC;
  if (!strcmp(s, "Spelling")) return KW_MNEMONIC;
  if (!strcmp(s, "Parameters")) return KW_PARAMETERS;
  if (!strcmp(s, "Methods")) return KW_METHODS;
  if (!strcmp(s, "Method")) return KW_METHOD;
  if (!strcmp(s, "Namespace")) return KW_NAMESPACE;
  if (!strcmp(s, "Class")) return KW_CLASS;
  if (!strcmp(s, "Dependent")) return KW_DEPENDENT;
  return IDENT;
}

static int format_kw(const char *s) {
  if (!strcmp(s, "attr-dict")) return F_ATTR_DICT;
  if (!strcmp(s, "prop-dict")) return F_PROP_DICT;
  if (!strcmp(s, "type")) return F_TYPE;
  if (!strcmp(s, "functional-type")) return F_FUNCTIONAL_TYPE;
  if (!strcmp(s, "operands")) return F_OPERANDS;
  if (!strcmp(s, "results")) return F_RESULTS;
  if (!strcmp(s, "regions")) return F_REGIONS;
  if (!strcmp(s, "successors")) return F_SUCCESSORS;
  if (!strcmp(s, "custom")) return F_CUSTOM;
  if (!strcmp(s, "ref")) return F_REF;
  if (!strcmp(s, "enum")) return F_ENUM;
  if (!strcmp(s, "qualified")) return F_QUALIFIED;
  if (!strcmp(s, "oilist")) return F_OILIST;
  if (!strcmp(s, "params")) return F_PARAMS;
  if (!strcmp(s, "struct")) return F_STRUCT;
  if (!strcmp(s, "if")) return KW_IF;
  if (!strcmp(s, "else")) return KW_ELSE;
  return IDENT;
}

int yylex(void) {
  for (;;) {
    int c = getch();
    if (c == EOF) return 0;
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;

    if (c == '/') {
      int d = getch();
      if (d == '/') {
        int e = getch();
        if (e == '/' && mode == 0) {
          while ((c = getch()) != EOF && c != '\n') {}
          return DOC;
        }
        ungetch(e);
        while ((c = getch()) != EOF && c != '\n') {}
        continue;
      }
      if (d == '*') {
        int p = 0;
        while ((c = getch()) != EOF) { if (p == '*' && c == '/') break; p = c; }
        continue;
      }
      ungetch(d);
      return c;
    }

    if (mode == 1) {
      if (c == '"') return read_string();
      if (isalpha((unsigned char)c) || c == '_') { read_ident(c, 1); return format_kw(ident); }
      switch (c) {
        case '{': fmt_depth++; return L_BRACE;
        case '}': if (--fmt_depth == 0) mode = 0; return R_BRACE;
        case '[': return L_BRACKET;
        case ']': return R_BRACKET;
        case '<': return L_ANGLE;
        case '>': return R_ANGLE;
        case ',': return COMMA;
        default: return c;
      }
    }

    if (c == '"') { expect_format = 0; return read_string(); }
    if (isdigit((unsigned char)c)) {
      int v = c - '0';
      while ((c = getch()) != EOF && isdigit((unsigned char)c)) v = v * 10 + (c - '0');
      ungetch(c);
      expect_format = 0;
      return INT;
    }
    if (isalpha((unsigned char)c) || c == '_') {
      read_ident(c, 0);
      if (!strcmp(ident, "code")) {
        int d;
        do { d = getch(); } while (d == ' ' || d == '\t' || d == '\r' || d == '\n');
        if (d == '{') { expect_format = 0; return scan_code(); }
        ungetch(d);
        return IDENT;
      }
      int t = normal_kw(ident);
      if (t == KW_FORMAT) expect_format = 1;
      else expect_format = 0;
      return t;
    }

    if (c == '{' && expect_format) {
      expect_format = 0;
      mode = 1;
      fmt_depth = 1;
      return L_BRACE;
    }
    expect_format = 0;
    switch (c) {
      case '{': return L_BRACE;
      case '}': return R_BRACE;
      case '(': return L_PAREN;
      case ')': return R_PAREN;
      case '[': return L_BRACKET;
      case ']': return R_BRACKET;
      case '<': return L_ANGLE;
      case '>': return R_ANGLE;
      case ',': return COMMA;
      case ':': return COLON;
      case ';': return SEMI;
      case '=': return EQUAL;
      case '.': return DOT;
      case '|': return PIPE;
      default: return c;
    }
  }
}

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: %s file.mdl\n", argv[0]); return 2; }
  src = fopen(argv[1], "r");
  if (!src) { perror(argv[1]); return 2; }
  if (getenv("DSL_DEBUG")) yydebug = 1;
  int r = yyparse();
  if (r == 0) printf("parse OK: %s\n", argv[1]);
  return r;
}
