// Minimal lexer + driver for legalize-decl.v7.y.
// The grammar is the spec; this file only exists so the grammar can be
// *executed* on real sentences.  A grammar that compiles but rejects the
// declarations it was written for is not a grammar.
//
// build: bison -Wall -d -o build/v7.cc legalize-decl.v7.y
//        g++ -std=c++17 -Ibuild -o build/parse_check tools/parse_check.cc build/v7.cc
// run:   build/parse_check tests/v7-shapes.decl

#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#include "v7.hh"   // bison -d output

static std::string Src;
static size_t Pos = 0;
static int Tokens = 0;

extern int yyparse();

void yyerror(const char *Msg) {
  size_t Line = 1, Col = 1;
  for (size_t I = 0; I < Pos && I < Src.size(); ++I) {
    if (Src[I] == '\n') { ++Line; Col = 1; } else { ++Col; }
  }
  std::fprintf(stderr, "parse error at %zu:%zu: %s\n", Line, Col, Msg);
}

static void Skip() {
  for (;;) {
    while (Pos < Src.size() && std::isspace((unsigned char)Src[Pos])) ++Pos;
    if (Pos + 1 < Src.size() && Src[Pos] == '/' && Src[Pos + 1] == '/') {
      while (Pos < Src.size() && Src[Pos] != '\n') ++Pos;
      continue;
    }
    if (Pos + 1 < Src.size() && Src[Pos] == '/' && Src[Pos + 1] == '*') {
      Pos += 2;
      while (Pos + 1 < Src.size() && !(Src[Pos] == '*' && Src[Pos + 1] == '/')) ++Pos;
      Pos = (Pos + 1 < Src.size()) ? Pos + 2 : Src.size();
      continue;
    }
    return;
  }
}

static bool IsIdentStart(char C) { return std::isalpha((unsigned char)C) || C == '_'; }
static bool IsIdentChar(char C) { return std::isalnum((unsigned char)C) || C == '_'; }
static bool IsDigit(char C) { return std::isdigit((unsigned char)C) != 0; }

static const std::map<std::string, int> &Keywords() {
  static const std::map<std::string, int> K = {
      {"op", OP}, {"macro", MACRO}, {"legal", LEGAL}, {"prefer", PREFER},
      {"follow", FOLLOW}, {"except", EXCEPT}, {"when", WHEN}, {"on", ON},
      {"from", FROM}, {"by", BY}, {"set", SET}, {"split", SPLIT},
      {"delegate", DELEGATE}, {"unsupported", UNSUPPORTED},
      {"ty", TY}, {"mem", MEM}, {"opc", OPC}, {"kind", KIND}, {"bits", BITS},
      {"elem", ELEM}, {"lanes", LANES}, {"addrspace", ADDRSPACE}, {"size", SIZE},
      {"class", CLASS}, {"elemwise", ELEMWISE}, {"shapes", SHAPES},
      {"any", ANY}, {"never", NEVER}, {"cond", COND}, {"pow2", POW2},
      {"min", MIN}, {"scalar", SCALAR}, {"vector", VECTOR},
      {"pointer", POINTER}, {"ptrvector", PTRVECTOR}, {"true", TRUE},
      {"false", FALSE}, {"in", IN}, {"query", QUERY},
      {"variadic", VARIADIC}, {"policy", POLICY}, {"imm", IMM}, {"to", TO},
      {"hull", HULL}, {"promote", PROMOTE}, {"truncate", TRUNCATE},
  };
  return K;
}

// TYPESPEC := s<N> | p<A>:<N> | v<L>s<N> | v<L>p<A>:<N>
static bool LexTypeSpec(size_t &I, size_t &End) {
  size_t P = I;
  auto Num = [&](size_t &Q) {
    size_t S = Q;
    while (Q < Src.size() && IsDigit(Src[Q])) ++Q;
    return Q > S;
  };
  if (P < Src.size() && Src[P] == 'v') {
    ++P;
    if (!Num(P)) return false;
    if (P < Src.size() && Src[P] == 's') {
      ++P;
      if (!Num(P)) return false;
      End = P;
      return true;
    }
    if (P < Src.size() && Src[P] == 'p') {
      ++P;
      if (!Num(P) || P >= Src.size() || Src[P] != ':') return false;
      ++P;
      if (!Num(P)) return false;
      End = P;
      return true;
    }
    return false;
  }
  if (P < Src.size() && Src[P] == 'p') {
    ++P;
    if (!Num(P) || P >= Src.size() || Src[P] != ':') return false;
    ++P;
    if (!Num(P)) return false;
    End = P;
    return true;
  }
  if (P < Src.size() && Src[P] == 's') {
    ++P;
    if (!Num(P)) return false;
    End = P;
    return true;
  }
  return false;
}

int yylex() {
  Skip();
  if (Pos >= Src.size()) return 0;
  ++Tokens;
  char C = Src[Pos];

  // Multi-char punctuation, longest first.
  struct { const char *Text; int Tok; } Punct[] = {
      {"->", ARROW}, {"&&", ANDAND}, {"||", OROR}, {"!=", NE}, {"==", EQ},
      {"<=", LE}, {">=", GE}, {"<", LT}, {">", GT}, {"=", EQ}, {"!", BANG},
      {"+", PLUS}, {"-", MINUS}, {"*", STAR}, {"/", SLASH}, {"{", LBRACE},
      {"}", RBRACE}, {"(", LPAREN}, {")", RPAREN}, {",", COMMA}, {";", SEMI},
      {":", COLON}, {".", DOT}, {"[", LBRACKET}, {"]", RBRACKET},
  };
  for (auto &P : Punct) {
    size_t N = std::strlen(P.Text);
    if (Src.compare(Pos, N, P.Text) == 0) {
      Pos += N;
      return P.Tok;
    }
  }
  if (C == '_' && (Pos + 1 >= Src.size() || !IsIdentChar(Src[Pos + 1]))) {
    ++Pos;
    return UNDERSCORE;
  }
  if (IsDigit(C)) {
    while (Pos < Src.size() && IsDigit(Src[Pos])) ++Pos;
    return INT;
  }
  if (C == 'G' && Pos + 1 < Src.size() && Src[Pos + 1] == '_') {
    size_t P = Pos + 2;
    while (P < Src.size() && IsIdentChar(Src[P])) ++P;
    Pos = P;
    return OPCODE;
  }
  if (IsIdentStart(C)) {
    size_t P = Pos, End = Pos;
    while (P < Src.size() && IsIdentChar(Src[P])) ++P;
    std::string Word = Src.substr(Pos, P - Pos);
    auto It = Keywords().find(Word);
    if (It != Keywords().end()) {
      Pos = P;
      return It->second;
    }
    // Typespec wins over IDENT when it matches: `p0:64` has a ':' that the
    // identifier scanner stops at, so the word-based test cannot decide it.
    size_t TEnd = Pos;
    if (LexTypeSpec(Pos, TEnd)) {
      Pos = TEnd;
      return TYPESPEC;
    }
    Pos = P;
    return IDENT;
  }
  std::fprintf(stderr, "stray character '%c' at %zu\n", C, Pos);
  ++Pos;
  return 0;
}

int main(int Argc, char **Argv) {
  if (Argc != 2) {
    std::fprintf(stderr, "usage: %s <file.decl>\n", Argv[0]);
    return 2;
  }
  FILE *F = std::fopen(Argv[1], "rb");
  if (!F) { std::perror(Argv[1]); return 2; }
  char Buf[4096];
  size_t N;
  while ((N = std::fread(Buf, 1, sizeof(Buf), F)) > 0) Src.append(Buf, N);
  std::fclose(F);
  Pos = 0;
  int Rc = yyparse();
  if (Rc == 0)
    std::printf("ok  %s  (%zu bytes, %d tokens)\n", Argv[1], Src.size(), Tokens);
  return Rc == 0 ? 0 : 1;
}
