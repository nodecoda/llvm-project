//===- LegalizerCoreEval.cpp - Evaluate legalizer rules at build time -----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "LegalizerCoreEval.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MathExtras.h"

using namespace llvm;
using namespace llvm::legeval;

namespace {

//===----------------------------------------------------------------------===//
// The serialized predicate
//===----------------------------------------------------------------------===//

void skipSpace(StringRef S, size_t &I) {
  while (I != S.size() && isSpace(S[I]))
    ++I;
}

bool expect(StringRef S, size_t &I, char C, std::string &Err) {
  skipSpace(S, I);
  if (I == S.size() || S[I] != C) {
    Err = std::string("expected '") + C + "' at offset " + std::to_string(I);
    return false;
  }
  ++I;
  return true;
}

bool parseString(StringRef S, size_t &I, std::string &Out, std::string &Err) {
  skipSpace(S, I);
  if (I == S.size() || S[I] != '"') {
    Err = "expected a string at offset " + std::to_string(I);
    return false;
  }
  ++I;
  while (I != S.size() && S[I] != '"') {
    if (S[I] == '\\' && I + 1 != S.size()) {
      ++I;
      Out.push_back(S[I] == 'n' ? '\n' : S[I]);
      ++I;
      continue;
    }
    Out.push_back(S[I++]);
  }
  if (I == S.size()) {
    Err = "unterminated string";
    return false;
  }
  ++I;
  return true;
}

bool parseArg(StringRef S, size_t &I, Arg &A, std::string &Err) {
  skipSpace(S, I);
  if (I == S.size()) {
    Err = "expected an argument";
    return false;
  }
  if (S[I] == '"') {
    A.K = Arg::Str;
    return parseString(S, I, A.S, Err);
  }
  if (S[I] == '[') {
    A.K = Arg::StrList;
    ++I;
    skipSpace(S, I);
    if (I != S.size() && S[I] == ']') {
      ++I;
      return true;
    }
    for (;;) {
      std::string E;
      if (!parseString(S, I, E, Err))
        return false;
      A.L.push_back(E);
      skipSpace(S, I);
      if (I != S.size() && S[I] == ',') {
        ++I;
        continue;
      }
      return expect(S, I, ']', Err);
    }
  }
  A.K = Arg::Int;
  bool Neg = false;
  if (S[I] == '-') {
    Neg = true;
    ++I;
  }
  size_t Begin = I;
  while (I != S.size() && isDigit(S[I]))
    ++I;
  if (I == Begin) {
    Err = "expected an integer at offset " + std::to_string(Begin);
    return false;
  }
  A.I = std::stoll(S.slice(Begin, I).str());
  if (Neg)
    A.I = -A.I;
  return true;
}

//===----------------------------------------------------------------------===//
// theta
//===----------------------------------------------------------------------===//

class BoolParser {
  StringRef S;
  size_t I = 0;
  const Theta &T;
  std::string &Err;

  void skip() {
    while (I != S.size() && isSpace(S[I]))
      ++I;
  }

  bool parseOr(bool &V) {
    if (!parseAnd(V))
      return false;
    for (;;) {
      skip();
      if (!S.substr(I).starts_with("||"))
        return true;
      I += 2;
      bool R;
      if (!parseAnd(R))
        return false;
      V = V || R;
    }
  }

  bool parseAnd(bool &V) {
    if (!parseUnary(V))
      return false;
    for (;;) {
      skip();
      if (!S.substr(I).starts_with("&&"))
        return true;
      I += 2;
      bool R;
      if (!parseUnary(R))
        return false;
      V = V && R;
    }
  }

  bool parseUnary(bool &V) {
    skip();
    if (I != S.size() && S[I] == '!') {
      ++I;
      bool R;
      if (!parseUnary(R))
        return false;
      V = !R;
      return true;
    }
    if (I != S.size() && S[I] == '(') {
      ++I;
      if (!parseOr(V))
        return false;
      skip();
      if (I == S.size() || S[I] != ')') {
        Err = "unbalanced parentheses in a feature condition";
        return false;
      }
      ++I;
      return true;
    }
    if (I != S.size() && (S[I] == '1' || S[I] == '0')) {
      V = S[I] == '1';
      ++I;
      return true;
    }
    if (I != S.size() && (isAlpha(S[I]) || S[I] == '_')) {
      size_t Begin = I;
      while (I != S.size() && (isAlnum(S[I]) || S[I] == '_'))
        ++I;
      bool Ok = false;
      V = T.evalName(S.slice(Begin, I), Ok);
      if (!Ok) {
        Err = "feature condition '" + S.slice(Begin, I).str() +
              "' is not one of the declared booleans";
        return false;
      }
      return true;
    }
    Err = "unexpected character in a feature condition at offset " +
          std::to_string(I);
    return false;
  }

public:
  BoolParser(StringRef Text, const Theta &T, std::string &Err)
      : S(Text), T(T), Err(Err) {}

  bool run(bool &V) {
    if (!parseOr(V))
      return false;
    skip();
    if (I != S.size()) {
      Err =
          "trailing text in a feature condition at offset " + std::to_string(I);
      return false;
    }
    return true;
  }
};

/// Split "cond ? then : else" at the top level.  A conditional nested in the
/// "then" arm is refused rather than guessed; nesting in the "else" arm is
/// right-associative and handled by the recursive caller, exactly as the
/// emitter writes it.
bool splitConditional(StringRef S, StringRef &Cond, StringRef &Then,
                      StringRef &Else) {
  unsigned Depth = 0;
  size_t Q = StringRef::npos;
  for (size_t I = 0; I != S.size(); ++I) {
    if (S[I] == '(')
      ++Depth;
    else if (S[I] == ')')
      --Depth;
    else if (S[I] == '?' && Depth == 0) {
      Q = I;
      break;
    }
  }
  if (Q == StringRef::npos)
    return false;
  Depth = 0;
  unsigned Pending = 0;
  for (size_t I = Q + 1; I != S.size(); ++I) {
    if (S[I] == '(')
      ++Depth;
    else if (S[I] == ')')
      --Depth;
    else if (S[I] == '?' && Depth == 0)
      ++Pending;
    else if (S[I] == ':' && Depth == 0) {
      if (Pending) {
        --Pending;
        continue;
      }
      Cond = S.slice(0, Q);
      Then = S.slice(Q + 1, I);
      Else = S.substr(I + 1);
      return true;
    }
  }
  return false;
}

StringRef stripParens(StringRef S) {
  S = S.trim();
  while (S.size() >= 2 && S.front() == '(' && S.back() == ')') {
    // Only strip when the first parenthesis closes at the end.
    unsigned Depth = 0;
    bool ClosesAtEnd = true;
    for (size_t I = 0; I != S.size(); ++I) {
      if (S[I] == '(')
        ++Depth;
      else if (S[I] == ')') {
        --Depth;
        if (Depth == 0 && I + 1 != S.size()) {
          ClosesAtEnd = false;
          break;
        }
      }
    }
    if (!ClosesAtEnd)
      break;
    S = S.drop_front().drop_back().trim();
  }
  return S;
}

} // namespace

void llvm::legeval::Theta::add(StringRef Name, StringRef Condition, bool Value) {
  ByName[Name] = Value;
  StringRef C = Condition.trim();
  if (C.empty())
    return;
  ByCondition.emplace_back(C.str(), Value);
  llvm::sort(ByCondition, [](const auto &A, const auto &B) {
    return A.first.size() > B.first.size();
  });
}

bool llvm::legeval::Theta::evalName(StringRef Name, bool &Ok) const {
  auto It = ByName.find(Name);
  Ok = It != ByName.end();
  return Ok && It->second;
}

bool llvm::legeval::Theta::eval(StringRef Text, bool &Ok) const {
  // Every declared condition is replaced by its value first, longest first:
  // the Core IR rewrites a record name to the condition text it stands for,
  // and a compound condition such as "UseX87 && !HasSSE1" is that same
  // substitution with the names spliced in.
  std::string Sub = Text.trim().str();
  for (const auto &C : ByCondition) {
    const std::string Rep = C.second ? "(1)" : "(0)";
    size_t P = 0;
    while ((P = Sub.find(C.first, P)) != std::string::npos) {
      Sub.replace(P, C.first.size(), Rep);
      P += Rep.size();
    }
  }
  std::string Why;
  BoolParser P(Sub, *this, Why);
  bool V = false;
  if (!P.run(V)) {
    Ok = false;
    return false;
  }
  Ok = true;
  return V;
}

bool llvm::legeval::parseNode(StringRef S, size_t &I, Node &N,
                              std::string &Err) {
  if (!expect(S, I, '{', Err))
    return false;
  for (;;) {
    skipSpace(S, I);
    if (I != S.size() && S[I] == '}') {
      ++I;
      return true;
    }
    std::string Key;
    if (!parseString(S, I, Key, Err))
      return false;
    if (!expect(S, I, ':', Err))
      return false;
    skipSpace(S, I);
    if (Key == "kind") {
      std::string V;
      if (!parseString(S, I, V, Err))
        return false;
      if (V == "leaf")
        N.K = Node::Leaf;
      else if (V == "not")
        N.K = Node::Not;
      else if (V == "nary")
        N.K = Node::All; // refined by "op"
      else {
        Err = "unknown predicate node kind '" + V + "'";
        return false;
      }
    } else if (Key == "name" || Key == "op") {
      std::string V;
      if (!parseString(S, I, V, Err))
        return false;
      if (Key == "op" && V != "any_of" && V != "all_of") {
        Err = "unknown n-ary predicate operator '" + V + "'";
        return false;
      }
      N.Name = V;
      if (Key == "op" && V == "any_of")
        N.K = Node::Any;
    } else if (Key == "arg") {
      Node Kid;
      if (!parseNode(S, I, Kid, Err))
        return false;
      N.Kids.push_back(std::move(Kid));
    } else if (Key == "args") {
      if (!expect(S, I, '[', Err))
        return false;
      skipSpace(S, I);
      if (I != S.size() && S[I] == ']') {
        ++I;
      } else {
        for (;;) {
          skipSpace(S, I);
          if (I != S.size() && S[I] == '{') {
            Node Kid;
            if (!parseNode(S, I, Kid, Err))
              return false;
            N.Kids.push_back(std::move(Kid));
          } else {
            Arg A;
            if (!parseArg(S, I, A, Err))
              return false;
            N.Args.push_back(std::move(A));
          }
          skipSpace(S, I);
          if (I != S.size() && S[I] == ',') {
            ++I;
            continue;
          }
          if (!expect(S, I, ']', Err))
            return false;
          break;
        }
      }
    } else {
      Err = "unexpected predicate key '" + Key + "'";
      return false;
    }
    skipSpace(S, I);
    if (I != S.size() && S[I] == ',')
      ++I;
  }
}

bool llvm::legeval::parseMutation(StringRef S, size_t &I, Mutation &M,
                                  std::string &Err) {
  auto Finish = [&]() -> bool {
    switch (M.K) {
    case Mutation::None:
      Err = "mutation has no kind";
      return false;
    case Mutation::ElemType:
    case Mutation::ElemBits:
      if (M.Text.empty()) {
        Err = "mutation has no value";
        return false;
      }
      return true;
    case Mutation::Lanes:
      if (!M.IsLiteral && M.Text.empty()) {
        Err = "lane mutation has no value";
        return false;
      }
      return true;
    }
    return false;
  };

  if (!expect(S, I, '{', Err))
    return false;
  for (;;) {
    skipSpace(S, I);
    if (I != S.size() && S[I] == '}') {
      ++I;
      return Finish();
    }
    std::string Key;
    if (!parseString(S, I, Key, Err))
      return false;
    if (!expect(S, I, ':', Err))
      return false;
    skipSpace(S, I);
    if (Key == "target") {
      Arg A;
      if (!parseArg(S, I, A, Err) || A.K != Arg::Int) {
        Err = "mutation target is not an integer";
        return false;
      }
      M.Target = static_cast<unsigned>(A.I);
    } else if (Key == "kind") {
      std::string V;
      if (!parseString(S, I, V, Err))
        return false;
      if (V == "set_elem_type")
        M.K = Mutation::ElemType;
      else if (V == "set_elem_bits")
        M.K = Mutation::ElemBits;
      else if (V == "set_lanes")
        M.K = Mutation::Lanes;
      else {
        Err = "unknown mutation kind '" + V + "'";
        return false;
      }
    } else if (Key == "value") {
      if (S.substr(I).starts_with("null")) {
        I += 4;
      } else {
        Arg A;
        if (!parseArg(S, I, A, Err))
          return false;
        if (A.K == Arg::Int) {
          M.IsLiteral = true;
          M.Literal = static_cast<uint64_t>(A.I);
        } else if (A.K == Arg::Str) {
          M.Text = A.S;
        } else {
          Err = "mutation value is a list";
          return false;
        }
      }
    } else {
      Err = "unexpected mutation key '" + Key + "'";
      return false;
    }
    skipSpace(S, I);
    if (I != S.size() && S[I] == ',')
      ++I;
  }
}

bool llvm::legeval::evalBound(const std::string &Text, const Env &E,
                              uint64_t &Out) {
  StringRef S = stripParens(Text);
  if (!S.empty() && S.find_first_not_of("0123456789") == StringRef::npos) {
    Out = std::stoull(S.str());
    return true;
  }
  StringRef Cond, Then, Else;
  if (!splitConditional(S, Cond, Then, Else))
    return false;
  bool Ok = false;
  bool V = E.Feature ? E.Feature(Cond, Ok) : false;
  if (!Ok)
    return false;
  return evalBound((V ? Then : Else).str(), E, Out);
}

namespace {

const LLT *named(const Env &E, StringRef Spell) {
  return E.NamedType ? E.NamedType(Spell) : nullptr;
}

bool fail(Env &E, const std::string &Why) {
  if (!E.Err->empty())
    return false;
  *E.Err = Why;
  return false;
}

uint64_t fixedOf(TypeSize T) { return T.isScalable() ? 0 : T.getFixedValue(); }

bool evalLeaf(const Node &N, Env &E) {
  const StringRef Name = N.Name;
  auto Slot = [&](size_t A) { return static_cast<unsigned>(N.Args[A].I); };
  auto TypeAt = [&](size_t A) -> const LLT & { return E.Types[Slot(A)]; };

  // A bound is an integer or the text of a conditional; both answer "how
  // many", and the conditional is resolved under theta.
  auto Bound = [&](size_t A) -> uint64_t {
    if (N.Args[A].K == Arg::Int)
      return static_cast<uint64_t>(N.Args[A].I);
    uint64_t V = 0;
    if (!evalBound(N.Args[A].S, E, V)) {
      fail(E, "bound '" + N.Args[A].S + "' has no value under theta");
      return 0;
    }
    return V;
  };

  if (Name == "true")
    return true;

  if (Name == "hasFeature") {
    bool Ok = false;
    bool V = E.Feature ? E.Feature(N.Args[0].S, Ok) : false;
    if (!Ok)
      return fail(E, "feature condition '" + N.Args[0].S + "' has no value");
    return V;
  }

  if (Name == "typeIs") {
    const LLT *T = named(E, N.Args[1].S);
    if (!T)
      return fail(E, "unknown type '" + N.Args[1].S + "'");
    return TypeAt(0) == *T;
  }
  if (Name == "typeIn") {
    const LLT &Q = TypeAt(0);
    for (const std::string &S : N.Args[1].L) {
      const LLT *T = named(E, S);
      if (!T)
        return fail(E, "unknown type '" + S + "'");
      if (Q == *T)
        return true;
    }
    return false;
  }
  if (Name == "isVector")
    return TypeAt(0).isVector();
  if (Name == "sizeIs")
    return fixedOf(TypeAt(0).getSizeInBits()) == Bound(1);
  if (Name == "sizeNotPow2")
    return TypeAt(0).isScalar() &&
           !llvm::has_single_bit<uint32_t>(fixedOf(TypeAt(0).getSizeInBits()));
  if (Name == "scalarOrEltSizeNotPow2")
    return !isPowerOf2_32(TypeAt(0).getScalarSizeInBits());
  if (Name == "numElementsNotPow2")
    return TypeAt(0).isFixedVector() &&
           !isPowerOf2_32(TypeAt(0).getNumElements());
  if (Name == "scalarNarrowerThan")
    return TypeAt(0).isScalar() &&
           fixedOf(TypeAt(0).getSizeInBits()) < Bound(1);
  if (Name == "scalarWiderThan")
    return TypeAt(0).isScalar() &&
           fixedOf(TypeAt(0).getSizeInBits()) > Bound(1);
  if (Name == "elementTypeIs") {
    const LLT *T = named(E, N.Args[1].S);
    if (!T)
      return fail(E, "unknown type '" + N.Args[1].S + "'");
    return TypeAt(0).isVector() && TypeAt(0).getElementType() == *T;
  }
  if (Name == "fewerElementsThan")
    return TypeAt(0).isFixedVector() && TypeAt(0).getNumElements() < Bound(1);
  if (Name == "moreElementsThan")
    return TypeAt(0).isFixedVector() && TypeAt(0).getNumElements() > Bound(1);
  // The two cross-slot leaves name the other slot first, which is the order
  // minScalarSameAs / maxScalarSameAs compare in.
  if (Name == "scalarSizeLargerThanSlot")
    return E.Types[Slot(0)].getScalarSizeInBits() >
           E.Types[Slot(1)].getScalarSizeInBits();
  if (Name == "scalarSizeSmallerThanSlot")
    return E.Types[Slot(0)].getScalarSizeInBits() <
           E.Types[Slot(1)].getScalarSizeInBits();
  // memSize(mmo, bits, align): TypePairAndMemDesc::isCompatible compares the
  // memory type by size only, and the alignment with >=.
  if (Name == "memSize")
    return E.HasMem && fixedOf(E.MemTy.getSizeInBits()) == Bound(1) &&
           E.MemAlignInBits >= Bound(2);
  if (Name == "typeDiffersFromMemDesc")
    return E.HasMem && TypeAt(0) != E.MemTy;

  return fail(E, "predicate leaf '" + N.Name + "' has no build-time meaning");
}

bool evalNode(const Node &N, Env &E) {
  switch (N.K) {
  case Node::Leaf:
    return evalLeaf(N, E);
  case Node::Not:
    if (N.Kids.size() != 1)
      return fail(E, "negation with " + std::to_string(N.Kids.size()) +
                         " operands");
    return !evalNode(N.Kids.front(), E);
  case Node::All:
    for (const Node &K : N.Kids)
      if (!evalNode(K, E))
        return false;
    return true;
  case Node::Any:
    for (const Node &K : N.Kids)
      if (evalNode(K, E))
        return true;
    return false;
  }
  llvm_unreachable("unknown predicate node kind");
}

/// The new type a matched rule's mutation produces, or why it has none.
///
/// Each branch mirrors the C++ mutation it names, including the pointer
/// restriction: changeElementSize() asserts on a pointer, so the evaluator
/// reports a hole instead of doing what the chain would not survive either.
bool applyMutation(const Mutation &M, const Env &E, LLT &NewTy,
                   std::string &Why) {
  if (M.Target >= E.Types.size()) {
    Why = "mutation targets slot " + std::to_string(M.Target) +
          ", which the query does not have";
    return false;
  }
  const LLT OldTy = E.Types[M.Target];

  switch (M.K) {
  case Mutation::None:
    return false;
  case Mutation::ElemType: {
    const LLT *Bound = named(E, M.Text);
    if (!Bound) {
      Why = "mutation names unknown type '" + M.Text + "'";
      return false;
    }
    if (OldTy.isPointerOrPointerVector()) {
      Why = "changeElementSize on a pointer type";
      return false;
    }
    NewTy = OldTy.changeElementSize(Bound->getScalarSizeInBits());
    return true;
  }
  case Mutation::ElemBits: {
    if (OldTy.isPointerOrPointerVector()) {
      Why = "changeElementSize on a pointer type";
      return false;
    }
    const unsigned OldBits = OldTy.getScalarSizeInBits();
    if (OldBits == 0) {
      Why = "next-pow-2 of a zero-sized scalar";
      return false;
    }
    unsigned NewBits = 0;
    StringRef T = M.Text;
    if (T == "nextPow2") {
      NewBits = 1u << Log2_32_Ceil(OldBits);
    } else if (T.starts_with("nextPow2OrMin")) {
      StringRef Digits = T.drop_front(StringRef("nextPow2OrMin").size());
      if (Digits.empty() ||
          Digits.find_first_not_of("0123456789") != StringRef::npos) {
        Why = "bit bound '" + M.Text + "' is not nextPow2OrMin<digits>";
        return false;
      }
      const unsigned Min = std::stoul(Digits.str());
      NewBits = std::max(1u << Log2_32_Ceil(OldBits), Min);
    } else if (T.starts_with("sameAsSlot")) {
      StringRef Digits = T.drop_front(StringRef("sameAsSlot").size());
      if (Digits.empty() ||
          Digits.find_first_not_of("0123456789") != StringRef::npos) {
        Why = "bit bound '" + M.Text + "' is not sameAsSlot<index>";
        return false;
      }
      const unsigned From = std::stoul(Digits.str());
      if (From >= E.Types.size()) {
        Why = "mutation reads slot " + std::to_string(From) +
              ", which the query does not have";
        return false;
      }
      NewBits = E.Types[From].getScalarSizeInBits();
    } else {
      Why = "bit bound '" + M.Text + "' has no build-time meaning";
      return false;
    }
    NewTy = OldTy.changeElementSize(NewBits);
    return true;
  }
  case Mutation::Lanes: {
    uint64_t N = M.Literal;
    if (!M.IsLiteral && !evalBound(M.Text, E, N)) {
      Why = "lane bound '" + M.Text + "' has no value under theta";
      return false;
    }
    // N == 1 is Scalarize: ElementCount::isScalar() is one element, so this
    // returns getScalarType(), i.e. the element type.
    NewTy = OldTy.changeElementCount(ElementCount::getFixed(N));
    return true;
  }
  }
  llvm_unreachable("unknown mutation kind");
}

} // namespace

Verdict llvm::legeval::apply(ArrayRef<Rule> Rules, const Env &EnvIn) {
  Verdict V;
  Env E = EnvIn;
  std::string Why;
  E.Err = &Why;
  for (const Rule &R : Rules) {
    Why.clear();
    bool Held = evalNode(R.Pred, E);
    if (!Why.empty()) {
      // The rule could not be evaluated at all: a hole in the description, not
      // a rule that did not match.
      V.Why = Why;
      return V;
    }
    if (Held) {
      V.Matched = true;
      V.Action = R.Action;
      V.TypeIdx = R.TypeIdx;
      V.MutKind = R.Mut.K;
      if (R.Mut.K != Mutation::None) {
        LLT NewTy;
        std::string MutWhy;
        if (!applyMutation(R.Mut, E, NewTy, MutWhy)) {
          // The rule won, but what it changes to is a hole in the
          // description.  Say so; do not invent a type.
          V.Why = MutWhy;
          return V;
        }
        V.HasNewType = true;
        V.NewType = NewTy;
      }
      return V;
    }
  }
  return V; // Ran off the end: no rule held.
}