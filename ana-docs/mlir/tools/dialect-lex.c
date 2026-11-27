/*===-- dialect-lex.c - reference lexer for dialect-grammar.y ---*- C -*-===//
 *
 * 参考词法器：只为证明 dialect-grammar.y 能解析 corpus/*.dialect，不是
 * 产品实现。两个模式：
 *   normal  —— 默认；`Assembly {` 之后切到 assembly 模式。
 *   assembly—— 字面量是 STRING；`attr-dict`/`attr-dict-with-keyword`/
 *              `prop-dict`/`type`/`if`/`functional-type`/`oilist`/`custom`/
 *              `Gap` 是关键字，
 *              其余标识符是部件引用；`{ }` 计深度，深度归零切回 normal。
 *
 * 名字允许 `.`（`reduce.return`），不允许 `-`；`attr-dict` 是 assembly 模式
 * 的特例。`type`/`if`/`else` 只在 assembly 模式里是关键字，所以普通模式里
 * `op if`、`type_def type` 合法。
 *
 * 构建与运行：
 *   bison -d -o /tmp/dg.c ana-docs/mlir/dialect-grammar.y
 *   cc -I/tmp -o /tmp/dg /tmp/dg.c ana-docs/mlir/tools/dialect-lex.c
 *   /tmp/dg ana-docs/mlir/corpus/arith.dialect
 *===----------------------------------------------------------------------===*/

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dg.h"

static FILE *src;
static int line = 1;
static int mode = 0;      /* 0 normal, 1 assembly */
static int asm_depth = 0;
static int expect_asm = 0;
static char ident[512];

void yyerror(const char *s) { fprintf(stderr, "line %d: %s\n", line, s); }

static int getch(void) {
  int c = fgetc(src);
  if (c == '\n') line++;
  return c;
}

static void ungetch(int c) {
  if (c != EOF) {
    if (c == '\n') line--;
    ungetc(c, src);
  }
}

static void read_ident(int first, int allow_dot) {
  int n = 0, c;
  ident[n++] = (char)first;
  while ((c = getch()) != EOF) {
    if (isalnum((unsigned char)c) || c == '_' || (allow_dot && c == '.')) {
      if (n < 511) ident[n++] = (char)c;
    } else {
      ungetch(c);
      break;
    }
  }
  ident[n] = 0;
}

static int read_string(void) {
  int c;
  while ((c = getch()) != EOF) {
    if (c == '\\') { (void)getch(); continue; }
    if (c == '"') break;
  }
  return STRING;
}

static int normal_kw(const char *s) {
  if (!strcmp(s, "dialect")) return KW_DIALECT;
  if (!strcmp(s, "op")) return KW_OP;
  if (!strcmp(s, "schema")) return KW_SCHEMA;
  if (!strcmp(s, "constraint")) return KW_CONSTRAINT;
  if (!strcmp(s, "enum")) return KW_ENUM;
  if (!strcmp(s, "attr_def")) return KW_ATTR_DEF;
  if (!strcmp(s, "type_def")) return KW_TYPE_DEF;
  if (!strcmp(s, "from")) return KW_FROM;
  if (!strcmp(s, "extends")) return KW_EXTENDS;
  if (!strcmp(s, "bits")) return KW_BITS;
  if (!strcmp(s, "optional")) return KW_OPTIONAL;
  if (!strcmp(s, "variadic")) return KW_VARIADIC;
  if (!strcmp(s, "Doc")) return KW_DOC;
  if (!strcmp(s, "Operands")) return KW_OPERANDS;
  if (!strcmp(s, "Results")) return KW_RESULTS;
  if (!strcmp(s, "Attributes")) return KW_ATTRIBUTES;
  if (!strcmp(s, "Regions")) return KW_REGIONS;
  if (!strcmp(s, "Successors")) return KW_SUCCESSORS;
  if (!strcmp(s, "Properties")) return KW_PROPERTIES;
  if (!strcmp(s, "Implements")) return KW_IMPLEMENTS;
  if (!strcmp(s, "Assembly")) return KW_ASSEMBLY;
  if (!strcmp(s, "Custom")) return KW_CUSTOM;
  if (!strcmp(s, "Generic")) return KW_GENERIC;
  if (!strcmp(s, "Cases")) return KW_CASES;
  if (!strcmp(s, "Groups")) return KW_GROUPS;
  if (!strcmp(s, "Parameters")) return KW_PARAMETERS;
  return IDENT;
}

static int asm_kw(const char *s) {
  if (!strcmp(s, "attr-dict")) return A_ATTR_DICT;
  if (!strcmp(s, "attr-dict-with-keyword")) return A_ATTR_DICT_WITH_KEYWORD;
  if (!strcmp(s, "prop-dict")) return A_PROP_DICT;
  if (!strcmp(s, "type")) return A_TYPE;
  if (!strcmp(s, "qualified")) return A_QUALIFIED;
  if (!strcmp(s, "if")) return A_IF;
  if (!strcmp(s, "functional-type")) return A_FUNCTIONAL_TYPE;
  if (!strcmp(s, "oilist")) return A_OILIST;
  if (!strcmp(s, "custom")) return A_CUSTOM;
  if (!strcmp(s, "Gap")) return A_GAP;   /* 大写：只在 Assembly 里，避开部件名 */
  if (!strcmp(s, "successors")) return A_SUCCESSORS;
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
      if (isalpha((unsigned char)c) || c == '_') {
        int n = 0, d;
        ident[n++] = (char)c;
        while ((d = getch()) != EOF) {
          if (isalnum((unsigned char)d) || d == '_' || d == '.' || d == '-') {
            if (n < 511) ident[n++] = (char)d;
          } else { ungetch(d); break; }
        }
        ident[n] = 0;
        return asm_kw(ident);
      }
      switch (c) {
        case '{': asm_depth++; return L_BRACE;
        case '}': if (--asm_depth == 0) mode = 0; return R_BRACE;
        case '(': return L_PAREN;
        case ')': return R_PAREN;
        case '[': return L_BRACKET;
        case ']': return R_BRACKET;
        case '<': return L_ANGLE;
        case '>': return R_ANGLE;
        case ',': return COMMA;
        case ':': return COLON;
        case '=': return EQUAL;
        case '.': return DOT;
        case '|': return PIPE;
        default: return c;
      }
    }

    if (c == '"') return read_string();
    if (isdigit((unsigned char)c)) {
      int v = c - '0';
      while ((c = getch()) != EOF && isdigit((unsigned char)c)) v = v * 10 + (c - '0');
      ungetch(c);
      return INT;
    }
    if (isalpha((unsigned char)c) || c == '_') {
      read_ident(c, 1);
      if (!strcmp(ident, "Assembly")) {
        int d;
        do { d = getch(); } while (d == ' ' || d == '\t' || d == '\r' || d == '\n');
        expect_asm = (d == '{');
        ungetch(d);
        return KW_ASSEMBLY;
      }
      return normal_kw(ident);
    }
    if (c == '{' && expect_asm) {
      expect_asm = 0;
      mode = 1;
      asm_depth = 1;
      return L_BRACE;
    }
    expect_asm = 0;
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
      case ';': return SEMI;
      case ':': return COLON;
      case '=': return EQUAL;
      case '|': return PIPE;
      case '.': return DOT;
      default: return c;
    }
  }
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s file.dialect...\n", argv[0]);
    return 2;
  }
  int rc = 0;
  for (int i = 1; i < argc; i++) {
    src = fopen(argv[i], "r");
    if (!src) { perror(argv[i]); return 2; }
    line = 1; mode = 0; asm_depth = 0; expect_asm = 0;
    if (getenv("DIALECT_DEBUG")) yydebug = 1;
    if (yyparse() == 0)
      printf("parse OK: %s\n", argv[i]);
    else
      rc = 1;
    fclose(src);
  }
  return rc;
}
