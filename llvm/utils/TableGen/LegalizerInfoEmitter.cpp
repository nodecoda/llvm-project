//===- LegalizerInfoEmitter.cpp - Emit legalizer rule fragments -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Emit GlobalISel legalizer rule chains from TableGen.
//
// The output is a statement sequence (not a data table), meant to be included
// inside a target's LegalizerInfo constructor, e.g.
//
//   getActionDefinitionsBuilder(G_CTPOP)
//       .legalFor(HasPOPCNT, {{s16, s16}, {s32, s32}})
//       .clampScalar(1, s16, sMaxScalar)
//       .scalarSameSizeAs(0, 1);
//
// The checks below are the point of the exercise: they are generation-time
// checks that the hand-written C++ cannot express, because there the predicate
// is an opaque std::function.
//
//===----------------------------------------------------------------------===//

#include "Basic/PredicateExpanderDag.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TableGen/Error.h"
#include "llvm/TableGen/Record.h"
#include "llvm/TableGen/TableGenBackend.h"

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <string>

using namespace llvm;

namespace {

static void skipSpaces(StringRef S, size_t &I) {
  while (I != S.size() && isSpace(S[I]))
    ++I;
}

/// Parse one operand: an identifier, a negation, or a parenthesized
/// combination.  Returns false on anything else - a call, a comparison, a
/// literal.
static bool parseBoolOperand(StringRef S, size_t &I,
                             SmallVectorImpl<StringRef> &Idents) {
  skipSpaces(S, I);
  if (I == S.size())
    return false;
  if (S[I] == '!') {
    ++I;
    return parseBoolOperand(S, I, Idents);
  }
  if (S[I] == '(') {
    ++I;
    if (!parseBoolOperand(S, I, Idents))
      return false;
    skipSpaces(S, I);
    if (I == S.size() || S[I] != ')')
      return false;
    ++I;
    return true;
  }
  if (!isAlpha(S[I]) && S[I] != '_')
    return false;
  size_t Begin = I;
  while (I != S.size() && (isAlnum(S[I]) || S[I] == '_'))
    ++I;
  Idents.push_back(S.slice(Begin, I));
  return true;
}

/// True if \p Expr is a boolean combination of identifiers and nothing else,
/// e.g. "HasSSE1 || UseX87".  Anything else - a call, a comparison, a literal -
/// is what LegalizeOpaqueBool exists for.  Precedence is irrelevant here: the
/// only question is whether the text could have been written as a
/// LegalizePredicateDag with the all_of / any_of / none_of / not operators.
static bool isBooleanCombination(StringRef Expr,
                                 SmallVectorImpl<StringRef> &Idents) {
  size_t I = 0;
  if (!parseBoolOperand(Expr, I, Idents))
    return false;
  for (;;) {
    size_t Save = I;
    skipSpaces(Expr, I);
    StringRef Rest = Expr.substr(I);
    if (!Rest.consume_front("&&") && !Rest.consume_front("||")) {
      I = Save;
      break;
    }
    I = Expr.size() - Rest.size();
    if (!parseBoolOperand(Expr, I, Idents))
      return false;
  }
  skipSpaces(Expr, I);
  return I == Expr.size();
}

/// Print a verbatim C++ body - the escape hatch for predicates that are still
/// raw lambdas - re-indented to line up under the builder call it belongs to.
/// TableGen code blocks carry the indentation of the .td file they came from,
/// which would otherwise produce badly aligned output.
void emitOpaqueBody(raw_ostream &OS, StringRef Body) {
  SmallVector<StringRef, 16> Lines;
  Body.split(Lines, '\n');
  // A .td checked out with core.autocrlf keeps a carriage return at the end of
  // every line, and a code block is copied verbatim.  The carriage return is
  // not part of the body.
  for (StringRef &Line : Lines)
    Line.consume_back("\r");

  unsigned Begin = 0, End = Lines.size();
  while (Begin != End && Lines[Begin].trim().empty())
    ++Begin;
  while (End != Begin && Lines[End - 1].trim().empty())
    --End;

  // Strip the indentation the body shares, so that only its internal relative
  // layout is preserved.
  unsigned Common = ~0u;
  for (unsigned I = Begin; I != End; ++I) {
    StringRef Line = Lines[I];
    if (Line.trim().empty())
      continue;
    Common = std::min<unsigned>(Common, Line.size() - Line.ltrim().size());
  }
  if (Common == ~0u)
    Common = 0;

  // The call itself is emitted at six columns; indent the body by four more.
  for (unsigned I = Begin; I != End; ++I)
    OS << "\n          "
       << Lines[I].drop_front(std::min<unsigned>(Common, Lines[I].size()));
}

class LegalizerInfoEmitter {
  const RecordKeeper &Records;

  /// Feature names a rule, an opaque body or a type expression refers to.  A
  /// feature nothing references is not emitted, and says so.
  StringSet<> UsedFeatures;

  /// The mutation directions a rule may carry.  "any" is for actions that
  /// expand to more than one applied action (clampScalar and friends), where
  /// the rule legitimately picks a direction at query time.
  static bool isKnownMutation(StringRef Mutation) {
    static constexpr StringLiteral Known[] = {
        "widen", "narrow", "split", "merge", "bitcast", "neutral", "any"};
    return Mutation.empty() || llvm::is_contained(Known, Mutation);
  }

  void emitFeatures(raw_ostream &OS);
  void emitPredicate(const Record &P, raw_ostream &OS);
  void emitTypes(raw_ostream &OS);
  void verifyRule(const Record &Rule);
  void verifyLattices();
  void emitArg(const Record &Arg, raw_ostream &OS);
  void emitArgList(raw_ostream &OS, const Record &Rule);
  void emitRuleSet(raw_ostream &OS, const Record &RuleSet);

  /// The rule sets, in source order, after the checks both outputs owe the
  /// description: an opcode belongs to one rule set, a rule set has opcodes and
  /// rules, and every rule passes verifyRule().
  std::vector<const Record *> validatedRuleSets();

  /// Every literal LLT the description can talk about, plus the pointer type
  /// the configuration decides.  Shared so the dump and the table cannot
  /// disagree about what the domain is.
  std::vector<std::string> domainTypes();

public:
  explicit LegalizerInfoEmitter(const RecordKeeper &RK) : Records(RK) {}
  void run(raw_ostream &OS);
  void runCore(raw_ostream &OS);
  void runTable(raw_ostream &OS);
};

} // namespace

/// Collapse whitespace runs into single spaces and trim, so that a feature's
/// declaration is one statement on one line.  TableGen rejects a literal
/// newline inside a string literal, so this only fires for a "\n" escape or for
/// a condition pasted in from a formatted block.
static std::string squashSpace(StringRef S) {
  std::string Out;
  bool PendingSpace = false;
  for (char C : S) {
    if (C == ' ' || C == '\n' || C == '\t' || C == '\r') {
      PendingSpace = !Out.empty();
      continue;
    }
    if (PendingSpace) {
      Out += ' ';
      PendingSpace = false;
    }
    Out += C;
  }
  return Out;
}

/// Collect the identifiers in a C++ expression, skipping the ones that are
/// followed by '(': those are calls, not references to a record.
static void collectIdentifiers(StringRef Expr,
                               SmallVectorImpl<StringRef> &Out) {
  auto IsBody = [](char C) { return isAlnum(C) || C == '_'; };
  size_t I = 0, E = Expr.size();
  while (I != E) {
    while (I != E && !(isAlpha(Expr[I]) || Expr[I] == '_'))
      ++I;
    size_t Begin = I;
    while (I != E && IsBody(Expr[I]))
      ++I;
    if (Begin == I)
      continue;
    StringRef Id = Expr.slice(Begin, I);
    size_t J = I;
    while (J != E && (Expr[J] == ' ' || Expr[J] == '\t' || Expr[J] == '\n'))
      ++J;
    if (J != E && Expr[J] == '(')
      continue;
    Out.push_back(Id);
  }
}

/// Parse the bit width out of a literal scalar type, `LLT::scalar(32)`.
///
/// Anything else - a vector, a pointer, an expression that picks a type from a
/// feature - returns false: the width is not a number that can be read, so a
/// check that wants a width has to say so rather than guess one.
static bool parseScalarWidth(StringRef Expr, unsigned &Width) {
  StringRef Rest = Expr.trim();
  if (!Rest.consume_front("LLT::scalar("))
    return false;
  if (!Rest.consume_back(")"))
    return false;
  return !Rest.getAsInteger(10, Width);
}

/// A predicate dag holds its leaves as defs; collect the features among them.
static void collectFeatureLeaves(const Init *I, StringSet<> &Out) {
  if (const auto *D = dyn_cast<DagInit>(I)) {
    for (const Init *Arg : D->getArgs())
      collectFeatureLeaves(Arg, Out);
    return;
  }
  if (const auto *Def = dyn_cast<DefInit>(I)) {
    const Record *R = Def->getDef();
    if (R->isSubClassOf("LegalizeFeature"))
      Out.insert(R->getName());
  }
}

/// Collect the features one predicate record refers to.
static void collectPredicateFeatures(const Record *P, StringSet<> &Out) {
  if (P->isSubClassOf("LegalizeFeature")) {
    Out.insert(P->getName());
    return;
  }
  if (P->isSubClassOf("LegalizeBoolPredicateDag") ||
      P->isSubClassOf("LegalizeQueryPredicateDag"))
    collectFeatureLeaves(P->getValueAsDag("CondDag"), Out);
}

/// Emit `bool <name> = <expr>;` for every reachable LegalizeFeature record, in
/// .td order.  The record name *is* the C++ identifier, which is why a rule can
/// refer to a feature by writing the record itself instead of a string.  A
/// feature that nothing references is skipped: it would only become an unused
/// variable in the generated constructor.
void LegalizerInfoEmitter::emitFeatures(raw_ostream &OS) {
  std::vector<const Record *> Features =
      Records.getAllDerivedDefinitions("LegalizeFeature");
  llvm::sort(Features, LessRecordByID());
  unsigned Emitted = 0;
  for (const Record *F : Features) {
    if (!UsedFeatures.contains(F->getName()))
      continue;
    OS << "  bool " << F->getName() << " = "
       << squashSpace(F->getValueAsString("Cond")) << ";\n";
    ++Emitted;
  }
  if (Emitted)
    OS << "\n";
}

/// Lower one predicate record to a C++ boolean expression.  A combination is
/// walked by the shared emitPredicateDag(); a leaf is either a cached bool
/// (LegalizeFeature) or verbatim text (LegalizeOpaqueBool and
/// LegalizeQueryPredicate).
void LegalizerInfoEmitter::emitPredicate(const Record &P, raw_ostream &OS) {
  if (P.isSubClassOf("LegalizeBoolPredicateDag") ||
      P.isSubClassOf("LegalizeQueryPredicateDag")) {
    // A query dag renders as the lambda the C++ builders take; a bool dag
    // renders as the expression `legalFor(bool, ...)` takes.  Same tree,
    // different C++, which is the whole reason the two classes exist.
    const bool IsQuery = P.isSubClassOf("LegalizeQueryPredicateDag");
    auto EmitLeaf = [&](const Init &Leaf, raw_ostream &OS) -> bool {
      const auto *DI = dyn_cast<DefInit>(&Leaf);
      if (!DI)
        PrintFatalError(&P, "predicate dag leaf '" + Leaf.getAsString() +
                                "' is not a record");
      const Record *R = DI->getDef();
      if (R->isSubClassOf("LegalizeFeature")) {
        OS << R->getName();
        return false;
      }
      if (R->isSubClassOf("LegalizeTypePredicate")) {
        // A type predicate is a LegalityPredicate -- a callable -- so it can
        // only appear where the builder takes one.  A bool dag renders as a
        // bare expression, and `legalFor(typeIs(0, s32), {s32})` matches no
        // overload; catching it here keeps the error on the .td line instead
        // of on generated code.
        if (!IsQuery)
          PrintFatalError(P.getLoc(),
                          "predicate dag leaf '" + R->getName() +
                              "' reads the query, so the dag has to be a "
                              "LegalizeQueryPredicateDag");
        // The call text names LLT records; the C++ is inside the target's own
        // scope, so it is written as it stands.  It has to be *called* too: a
        // type predicate yields a LegalityPredicate, i.e. a
        // std::function<bool(const LegalityQuery &)>, and writing it bare
        // leaves `&&` comparing a std::function against bool.  That conversion
        // is explicit operator bool, which is true whenever the function is
        // non-empty, so the guard silently degenerates into its feature parts.
        OS << R->getValueAsString("Call") << "(Query)";
        return false;
      }
      PrintFatalError(R->getLoc(), "predicate dag leaf '" + R->getName() +
                                       "' is not a LegalizeFeature or a "
                                       "LegalizeTypePredicate");
      return false;
    };
    if (IsQuery)
      OS << "[=](const LegalityQuery &Query) { return ";
    emitPredicateDag(&P, *P.getValueAsDag("CondDag"), /*ParenIfBinOp=*/false,
                     OS, EmitLeaf);
    if (IsQuery)
      OS << "; }";
    return;
  }
  if (P.isSubClassOf("LegalizeFeature")) {
    OS << P.getName();
    return;
  }
  if (P.isSubClassOf("LegalizeOpaqueBool") ||
      P.isSubClassOf("LegalizeQueryPredicate")) {
    OS << squashSpace(P.getValueAsString("Expr"));
    return;
  }
  PrintFatalError(&P, "unknown LegalizePredicate record");
}

/// Emit the LLT constants and named type lists, in .td order.  Like a feature,
/// the record name is the C++ identifier; a type declared after one that uses
/// it would generate code that does not compile, which is why the names are
/// also checked for declaration order (see run()).
void LegalizerInfoEmitter::emitTypes(raw_ostream &OS) {
  std::vector<const Record *> Types =
      Records.getAllDerivedDefinitions("LegalizeNamedType");
  llvm::sort(Types, LessRecordByID());
  for (const Record *T : Types)
    OS << "  const LLT " << T->getName() << " = " << T->getValueAsString("Expr")
       << ";\n";
  if (!Types.empty())
    OS << "\n";

  std::vector<const Record *> Lists =
      Records.getAllDerivedDefinitions("LegalizeNamedTypeList");
  llvm::sort(Lists, LessRecordByID());
  for (const Record *L : Lists) {
    SmallVector<StringRef, 8> Elems;
    for (const Record *E : L->getValueAsListOfDefs("Elems"))
      Elems.push_back(E->getName());
    OS << "  const std::initializer_list<LLT> " << L->getName() << " = {"
       << join(Elems, ", ") << "};\n";
  }
  if (!Lists.empty())
    OS << "\n";
}

/// C++ spelling of the builder a rule names.
///
/// The rule points at a LegalizeBuilder record, so this string cannot be
/// misspelled independently of the record that defines it: a name that does
/// not exist is a TableGen error before any backend runs.
StringRef builderRecordName(const Record &Rule) {
  const Record *B = Rule.getValueAsOptionalDef("Builder");
  if (!B)
    PrintFatalError(Rule.getLoc(), "legalizer rule is missing its Builder");
  return B->getName();
}

StringRef builderName(const Record &Rule) {
  const Record *B = Rule.getValueAsOptionalDef("Builder");
  if (!B)
    PrintFatalError(Rule.getLoc(), "legalizer rule is missing its Builder");
  if (!B->isSubClassOf("LegalizeBuilder"))
    PrintFatalError(B->getLoc(),
                    "'" + B->getName() + "' is not a LegalizeBuilder");
  return B->getValueAsString("Name");
}


/// A list made only of types, type pairs or memdesc tuples is one braced group
/// in C++ -- legalFor({s8, s16}), legalFor({{s32, s32}, ...}),
/// legalForTypesWithMemDesc({{s8, p0, s8, 1}, ...}).  Anything else is a plain
/// argument list in which each argument renders itself, which is what the
/// builders that take an index and two types need.
static bool isBracedGroup(ArrayRef<const Record *> Args) {
  if (Args.empty())
    return false;
  return all_of(Args, [](const Record *A) {
    return A->isSubClassOf("LegalizeNamedType") ||
           A->isSubClassOf("LegalizeInlineType") ||
           A->isSubClassOf("LegalizeTypePair") ||
           A->isSubClassOf("LegalizeMemDesc");
  });
}

/// The shape an argument record has, or null if the emitter has no name for it.
///
/// This is the inverse of the Formals lists in LegalizerInfo.td: the builder
/// says which shapes it declares per position, and this says what a rule
/// actually put there.  Comparing the two is what stops `.clampScalar({s32})`
/// (one argument, three declared) and `.lower({s32})` (three arguments, none
/// declared) from being written at all -- both used to be emitted and only
/// failed when the target was compiled.
static StringRef shapeOf(const Record &Arg) {
  if (Arg.isSubClassOf("LegalizeArgIndex"))
    return "ShapeIndex";
  if (Arg.isSubClassOf("LegalizeArgBits"))
    return "ShapeBits";
  if (Arg.isSubClassOf("LegalizeArgLanes") ||
      Arg.isSubClassOf("LegalizeRawArg"))
    return "ShapeLanes";
  if (Arg.isSubClassOf("LegalizeNamedType") ||
      Arg.isSubClassOf("LegalizeInlineType"))
    return "ShapeType";
  if (Arg.isSubClassOf("LegalizeTypePair"))
    return "ShapeTypePairList";
  if (Arg.isSubClassOf("LegalizeMemDesc"))
    return "ShapeMemDescList";
  if (Arg.isSubClassOf("LegalizeNamedTypeList") ||
      Arg.isSubClassOf("LegalizeInlineTypeList") ||
      Arg.isSubClassOf("LegalizeTypeListChoice"))
    return "ShapeTypeList";
  // A bare LegalizeArgInt is deliberately not accepted: it is the class whose
  // three meanings (slot, bit width, lane count) the subclasses exist to tell
  // apart, so letting it through as any one of them would leave the ambiguity
  // this check is here to remove.
  return "";
}

void LegalizerInfoEmitter::verifyRule(const Record &Rule) {
  StringRef Method = builderName(Rule);
  const Record *B = Rule.getValueAsDef("Builder");

  // The action belongs to the builder: the keyword already decides it, so a
  // rule that named one too could only repeat it or contradict it.  Both
  // emitters read the builder, so a contradiction used to produce two
  // different answers for the same sentence.
  const Record *Action = B->getValueAsDef("Action");
  if (!Action->isSubClassOf("LegalizeAction"))
    PrintFatalError(B->getLoc(),
                    "'" + Action->getName() + "' is not a LegalizeAction");

  // The builder declares which guard overloads it has, and the guard the rule
  // carries has to be one of them.  Without this a rule could hand a cached
  // bool to a builder that takes a callable (or to one with no overload at
  // all) and the fragment would only fail when the target was compiled.
  std::vector<const Record *> Guards = B->getValueAsListOfDefs("Guards");
  bool TakesBoolGuard = llvm::any_of(Guards, [](const Record *G) {
    return G->getName() == "GuardBool";
  });
  bool TakesQueryGuard = llvm::any_of(Guards, [](const Record *G) {
    return G->getName() == "GuardQuery";
  });
  if (const Record *P = Rule.getValueAsOptionalDef("Pred")) {
    bool IsQuery = P->isSubClassOf("LegalizeQueryPredicateBase");
    if (IsQuery ? !TakesQueryGuard : !TakesBoolGuard) {
      StringRef What = Guards.empty() ? "no guard"
                                      : "a different kind of guard";
      PrintFatalError(Rule.getLoc(), "legalizer builder '" + Method +
                                         "' takes " + What);
    }
  }

  // The arguments have to line up with a signature the builder declares.
  //
  // Formals is a list of overloads, so the rule matches if any one of them
  // lines up.  Args is a flat record list and a run of types becomes one braced
  // group in C++ (a type list, or a list of pairs/memdesc tuples), so a group
  // is compared as a whole position rather than element by element.
  //
  // This is what stops `.clampScalar({s32})` (one argument, three declared) and
  // `.lower({s32})` (one argument, none declared) from being written at all:
  // both used to be emitted and only failed when the target was compiled.
  if (Rule.getValueAsString("OpaqueArgs").empty()) {
    std::vector<const Record *> Args = Rule.getValueAsListOfDefs("Args");
    bool Braced = isBracedGroup(Args);

    // The shape the rule supplies, position by position.
    std::vector<StringRef> Supplied;
    if (Braced) {
      bool AllPairs = llvm::all_of(Args, [](const Record *A) {
        return A->isSubClassOf("LegalizeTypePair");
      });
      bool AllMemDesc = llvm::all_of(Args, [](const Record *A) {
        return A->isSubClassOf("LegalizeMemDesc");
      });
      Supplied.push_back(AllPairs ? "ShapeTypePairList"
                                  : AllMemDesc ? "ShapeMemDescList"
                                               : "ShapeType");
    } else {
      for (const Record *A : Args) {
        StringRef Got = shapeOf(*A);
        if (Got.empty())
          PrintFatalError(A, "unknown legalizer argument shape");
        Supplied.push_back(Got);
      }
    }

    // Formals is list<list<LegalizeArgShape>>, so each overload is itself a
    // ListInit; the inner elements are still records.
    const ListInit *Overloads = B->getValueAsListInit("Formals");
    bool Matched = false;
    std::string Want;
    for (const Init *O : Overloads->getElements()) {
      const auto *Inner = dyn_cast<ListInit>(O);
      if (!Inner)
        continue;
      if (!Want.empty())
        Want += " or ";
      Want += "(";

      std::vector<StringRef> Names;
      StringRef Sep;
      for (const Init *E : Inner->getElements()) {
        const auto *DI = dyn_cast<DefInit>(E);
        if (!DI)
          continue;
        Names.push_back(DI->getDef()->getName());
        Want += Sep.str();
        Want += Names.back().str();
        Sep = ", ";
      }
      Want += ")";

      if (Names != Supplied)
        continue;
      Matched = true;
      break;
    }

    if (!Matched)
      PrintFatalError(Rule.getLoc(),
                      "legalizer builder '" + Method + "' does not take these "
                      "arguments; declared " + Want);
  }

  StringRef Mutation = Rule.getValueAsString("Mutation");
  if (!isKnownMutation(Mutation))
    PrintFatalError(Rule.getLoc(), "'" + Mutation +
                                       "' is not a legalizer mutation "
                                       "direction");

  // This is the check that hand-written C++ cannot do: the action and the
  // mutation live in two different places there, and nothing ties them
  // together (this is exactly the bug class behind
  // https://github.com/llvm/llvm-project/issues/220802).
  // "any" on either side means unconstrained: composite actions accept either
  // direction, and a rule that picks one at query time says so with "any".
  StringRef Required = Action->getValueAsString("Mutation");
  if (!Required.empty() && Required != "any" && Mutation != Required &&
      Mutation != "any")
    PrintFatalError(Rule.getLoc(), "action '" +
                                       Action->getValueAsString("Name") +
                                       "' requires a '" + Required +
                                       "' mutation, but this "
                                       "rule carries '" +
                                       Mutation + "'");

  // An opaque body is the guard, so the builder must be one that takes a
  // guard: `.lower({...})` has no overload.
  if (!Rule.getValueAsString("OpaqueArgs").empty() && Guards.empty())
    PrintFatalError(Rule.getLoc(), "legalizer builder '" + Method +
                                       "' takes no guard; it cannot carry an "
                                       "opaque body");

  // The emitter prints an opaque body and never looks at Pred or Args, so a
  // rule that carries either alongside it would lose them without a word.
  if (!Rule.getValueAsString("OpaqueArgs").empty()) {
    if (Rule.getValueAsOptionalDef("Pred"))
      PrintFatalError(Rule.getLoc(),
                      "legalizer rule '" + Method +
                          "' has both a Pred and an opaque body; the predicate "
                          "would be dropped");
    if (!Rule.getValueAsListOfDefs("Args").empty())
      PrintFatalError(Rule.getLoc(),
                      "legalizer rule '" + Method +
                          "' has both Args and an opaque body; the arguments "
                          "would be dropped");
  }
}

/// The type lattice is the one thing a rule cannot state: which type is "next
/// wider".  Hand-written C++ computes that inside widenScalarToNextPow2, and
/// clampScalar's bound is applied at run time whatever it names, so a bound of
/// "no wider than s128" on a target that has no s128 is a clamp that never
/// fires - or fires on the wrong type.  With the order written down, the same
/// mistake is an error when the .td is read.
///
/// Only literal bounds can be decided here.  A bound computed from a feature is
/// checked by the types its expression names: every alternative has to be in
/// some chain, which is what makes the bound resolvable at all.
void LegalizerInfoEmitter::verifyLattices() {
  std::vector<const Record *> Lattices =
      Records.getAllDerivedDefinitions("LegalizeTypeLattice");
  // A target that has not written an order down gets no check: the order is
  // data the .td supplies, not something the emitter can invent.
  if (Lattices.empty())
    return;

  StringSet<> TypeNames;
  for (const Record *T : Records.getAllDerivedDefinitions("LegalizeNamedType"))
    TypeNames.insert(T->getName());

  // Every type the chains hold, and every width they go through.
  StringSet<> Members;
  SmallDenseSet<unsigned, 8> Widths;
  for (const Record *L : Lattices) {
    std::vector<const Record *> Order = L->getValueAsListOfDefs("Order");
    if (Order.size() < 2)
      PrintFatalError(L->getLoc(), "type lattice '" + L->getName() +
                                       "' holds fewer than two types; one type "
                                       "says only that it is legal, which a "
                                       "rule already says");
    unsigned Prev = 0;
    for (const Record *T : Order) {
      unsigned Width = 0;
      if (!parseScalarWidth(T->getValueAsString("Expr"), Width))
        PrintFatalError(T->getLoc(), "type lattice '" + L->getName() +
                                         "' holds '" + T->getName() +
                                         "', which is not LLT::scalar(N); a "
                                         "widening chain holds scalars");
      if (Prev && Width != 2 * Prev)
        PrintFatalError(T->getLoc(),
                        "type lattice '" + L->getName() + "' steps from " +
                            std::to_string(Prev) + " to " +
                            std::to_string(Width) +
                            " bits; the next power of two doubles, so a step "
                            "in the chain has to double");
      Prev = Width;
      Members.insert(T->getName());
      Widths.insert(Width);
    }
  }

  // A bound is a type position that says "no narrower" or "no wider".
  for (const Record *Rule : Records.getAllDerivedDefinitions("LegalizeRule")) {
    StringRef BuilderRec = builderRecordName(*Rule);
    std::vector<const Record *> Args = Rule->getValueAsListOfDefs("Args");

    SmallVector<unsigned, 2> Bounds;
    if (BuilderRec == "BuilderClampScalar" ||
        BuilderRec == "BuilderClampScalarOrElt")
      Bounds = {1, 2};
    else if (BuilderRec == "BuilderMinScalar" ||
             BuilderRec == "BuilderMaxScalar")
      Bounds = {1};
    for (unsigned I : Bounds) {
      // How many arguments a builder takes is verifyRule()'s business.
      if (I >= Args.size())
        continue;
      const Record &A = *Args[I];
      bool Named = A.isSubClassOf("LegalizeNamedType");
      StringRef Expr = A.getValueAsString(Named ? "Expr" : "Text");

      // A literal type is checked by name; one computed from a feature has no
      // single name, so every type its expression names is checked instead.
      if (Named && !Expr.contains('?')) {
        if (!Members.contains(A.getName()))
          PrintFatalError(Rule->getLoc(),
                          "'" + A.getName() + "' is a " + builderName(*Rule) +
                              " bound, but is in no LegalizeTypeLattice; a "
                              "bound has to name a type the target widens "
                              "through");
        continue;
      }
      if (unsigned Width = 0; parseScalarWidth(Expr, Width)) {
        if (!Widths.contains(Width))
          PrintFatalError(Rule->getLoc(), "'" + Expr + "' is a " +
                                              builderName(*Rule) +
                                              " bound, but no lattice holds " +
                                              std::to_string(Width) + " bits");
        continue;
      }
      SmallVector<StringRef, 4> Idents;
      collectIdentifiers(Expr, Idents);
      for (StringRef Id : Idents)
        if (TypeNames.contains(Id) && !Members.contains(Id))
          PrintFatalError(Rule->getLoc(),
                          "'" + Id + "' is named by the " + builderName(*Rule) +
                              " bound '" + Expr +
                              "', but is in no LegalizeTypeLattice");
    }

    // widenScalarToNextPow2's explicit minimum is a width, not a type, so it is
    // checked against the widths the chains hold.  With no minimum the default
    // is the narrowest width and there is nothing to check.
    if ((BuilderRec == "BuilderWidenScalarToNextPow2" ||
         BuilderRec == "BuilderWidenScalarOrEltToNextPow2") &&
        Args.size() > 1) {
      unsigned Min = (unsigned)Args[1]->getValueAsInt("Value");
      if (!Widths.contains(Min))
        PrintFatalError(Rule->getLoc(),
                        "widenScalarToNextPow2 minimum " + std::to_string(Min) +
                            " is not a width any LegalizeTypeLattice holds");
    }
  }
}

/// Render one argument.  Shapes that stand for a group of types brace
/// themselves, so a list of pairs reads {{a, b}, {c, d}}; the brace around the
/// whole list is added by emitArgList.
void LegalizerInfoEmitter::emitArg(const Record &Arg, raw_ostream &OS) {
  if (Arg.isSubClassOf("LegalizeNamedType") ||
      Arg.isSubClassOf("LegalizeNamedTypeList")) {
    OS << Arg.getName();
    return;
  }
  if (Arg.isSubClassOf("LegalizeInlineType") ||
      Arg.isSubClassOf("LegalizeTypeListChoice")) {
    OS << Arg.getValueAsString("Text");
    return;
  }
  if (Arg.isSubClassOf("LegalizeInlineTypeList")) {
    OS << "{";
    StringRef Sep;
    for (const Record *E : Arg.getValueAsListOfDefs("Elems")) {
      OS << Sep << E->getName();
      Sep = ", ";
    }
    OS << "}";
    return;
  }
  if (Arg.isSubClassOf("LegalizeTypePair")) {
    OS << "{" << Arg.getValueAsDef("Type0")->getName() << ", "
       << Arg.getValueAsDef("Type1")->getName() << "}";
    return;
  }
  if (Arg.isSubClassOf("LegalizeMemDesc")) {
    OS << "{" << Arg.getValueAsDef("Type0")->getName() << ", "
       << Arg.getValueAsDef("Type1")->getName() << ", "
       << Arg.getValueAsDef("MemTy")->getName() << ", "
       << Arg.getValueAsInt("Align") << "}";
    return;
  }
  if (Arg.isSubClassOf("LegalizeArgInt")) {
    StringRef Comment = Arg.getValueAsString("Comment");
    if (!Comment.empty())
      OS << "/*" << Comment << "=*/";
    OS << Arg.getValueAsInt("Value");
    return;
  }
  if (Arg.isSubClassOf("LegalizeRawArg")) {
    OS << Arg.getValueAsString("Text");
    return;
  }
  PrintFatalError(&Arg, "unknown LegalizeArg record");
}

/// Render one rule's arguments.  The layout is derived from the argument
/// shapes, so a .td has no way to spell a brace that does not match its
/// builder.
void LegalizerInfoEmitter::emitArgList(raw_ostream &OS, const Record &Rule) {
  std::vector<const Record *> Args = Rule.getValueAsListOfDefs("Args");
  bool Braced = isBracedGroup(Args);
  if (Braced)
    OS << "{";
  StringRef Sep;
  for (const Record *A : Args) {
    OS << Sep;
    emitArg(*A, OS);
    Sep = ", ";
  }
  if (Braced)
    OS << "}";
}

void LegalizerInfoEmitter::emitRuleSet(raw_ostream &OS, const Record &RuleSet) {
  std::vector<StringRef> Opcodes = RuleSet.getValueAsListOfStrings("Opcodes");
  if (Opcodes.empty())
    PrintFatalError(RuleSet.getLoc(), "legalizer rule set has no opcodes");

  std::vector<const Record *> Rules = RuleSet.getValueAsListOfDefs("Rules");
  if (Rules.empty())
    PrintFatalError(RuleSet.getLoc(), "legalizer rule set has no rules");

  for (StringRef Comment : RuleSet.getValueAsListOfStrings("Comments"))
    OS << "  // " << Comment << "\n";
  OS << "  getActionDefinitionsBuilder(";
  if (Opcodes.size() == 1)
    OS << Opcodes.front();
  else
    OS << "{" << join(Opcodes, ", ") << "}";
  OS << ")\n";

  for (size_t I = 0, E = Rules.size(); I != E; ++I) {
    const Record &Rule = *Rules[I];
    verifyRule(Rule);
    StringRef Opaque = Rule.getValueAsString("OpaqueArgs");
    OS << "      ." << builderName(Rule) << "(";
    if (Opaque.empty()) {
      if (const Record *Pred = Rule.getValueAsOptionalDef("Pred")) {
        emitPredicate(*Pred, OS);
        // The guard and the arguments share one argument list, so the comma
        // belongs between them and not after a guard with nothing after it.
        if (!Rule.getValueAsListOfDefs("Args").empty())
          OS << ", ";
      }
      emitArgList(OS, Rule);
    } else
      emitOpaqueBody(OS, Opaque);
    OS << (I + 1 == E ? ");\n" : ")\n");
  }
}

void LegalizerInfoEmitter::run(raw_ostream &OS) {
  emitSourceFileHeader("GlobalISel legalizer rule fragments", OS);

  // A feature leaf that names something the .td never defined is rejected by
  // TableGen itself ("Variable not defined"), which points at the .td line and
  // runs before this backend.  What is left to check is the escape hatch: an
  // opaque predicate spelled with nothing but declared features and boolean
  // operators could have been written with the dag form, and letting it
  // through would leave text that looks like a predicate but is opaque.
  StringSet<> Declared;
  for (const Record *F : Records.getAllDerivedDefinitions("LegalizeFeature"))
    Declared.insert(F->getName());

  // The same question has to be asked of both escape hatches: text that is
  // nothing but declared features and boolean operators is a predicate that
  // looks structured but is not, so it belongs in the AST (or, for a query
  // predicate, is a cached bool passed where a callable is expected).
  auto CheckOpaqueGuard = [&](StringRef Class, StringRef Fix) {
    for (const Record *E : Records.getAllDerivedDefinitions(Class)) {
      SmallVector<StringRef, 4> Idents;
      StringRef Expr = E->getValueAsString("Expr");
      if (!isBooleanCombination(Expr, Idents))
        continue;
      if (!all_of(Idents, [&](StringRef Id) {
            return Id == "true" || Id == "false" || Declared.contains(Id);
          }))
        continue;
      PrintFatalError(E->getLoc(), Twine(Class) + " '" + Expr +
                                       "' is a boolean combination of declared "
                                       "features; " +
                                       Fix);
    }
  };
  CheckOpaqueGuard("LegalizeOpaqueBool", "write it with all_of / any_of / not");
  CheckOpaqueGuard(
      "LegalizeQueryPredicate",
      "use a predicate the query can evaluate, e.g. typeIs(0, s32)");

  // A type is rendered by its record name, so it has to be a named def: an
  // inline `LegalizeNamedType<...>` would emit an identifier called
  // anonymous_0.  Checking the class itself covers every position a type can
  // appear in, including the elements of a type list.
  for (const Record *T : Records.getAllDerivedDefinitions("LegalizeNamedType"))
    if (T->isAnonymous())
      PrintFatalError(T, "a legalizer type has to be a named def, e.g. "
                         "`def s32 : LegalizeNamedType<...>`");

  // The fragment is emitted in .td order, so a record that names one declared
  // further down would be emitted before its declaration and the generated C++
  // would not compile.  The name sits inside a C++ expression that TableGen
  // cannot resolve, so only -gen-legalizer can see it.
  auto CheckDeclarationOrder = [&](StringRef Class, StringRef Field) {
    std::vector<const Record *> Recs = Records.getAllDerivedDefinitions(Class);
    StringMap<unsigned> DeclOrder;
    for (const Record *R : Recs)
      DeclOrder[R->getName()] = R->getID();
    for (const Record *R : Recs) {
      SmallVector<StringRef, 8> Idents;
      collectIdentifiers(R->getValueAsString(Field), Idents);
      for (StringRef Id : Idents) {
        auto It = DeclOrder.find(Id);
        if (It != DeclOrder.end() && It->second > R->getID())
          PrintFatalError(R->getLoc(),
                          "'" + Id + "' is used by '" + R->getName() +
                              "' but declared below it; the fragment is "
                              "emitted in .td order");
      }
    }
  };

  CheckDeclarationOrder("LegalizeNamedType", "Expr");
  CheckDeclarationOrder("LegalizeFeature", "Cond");

  verifyLattices();

  // Everything a feature name can be written in: a predicate dag's leaves, and
  // the C++ text the escape hatches carry.
  auto CollectUsed = [&](StringRef Text) {
    SmallVector<StringRef, 8> Idents;
    collectIdentifiers(Text, Idents);
    for (StringRef Id : Idents)
      if (Declared.contains(Id))
        UsedFeatures.insert(Id);
  };
  for (const Record *D :
       Records.getAllDerivedDefinitions("LegalizeBoolPredicateDag"))
    collectPredicateFeatures(D, UsedFeatures);
  for (const Record *D :
       Records.getAllDerivedDefinitions("LegalizeQueryPredicateDag"))
    collectPredicateFeatures(D, UsedFeatures);
  for (const Record *P : Records.getAllDerivedDefinitions("LegalizeRule")) {
    CollectUsed(P->getValueAsString("OpaqueArgs"));
    if (const Record *Pred = P->getValueAsOptionalDef("Pred"))
      collectPredicateFeatures(Pred, UsedFeatures);
  }
  for (const Record *E : Records.getAllDerivedDefinitions("LegalizeOpaqueBool"))
    CollectUsed(E->getValueAsString("Expr"));
  // A feature whose condition names another feature is a reference like any
  // other: without this, the named feature would be reported as unused and not
  // declared, and the generated bool would not compile.
  for (const Record *F : Records.getAllDerivedDefinitions("LegalizeFeature"))
    CollectUsed(F->getValueAsString("Cond"));
  for (const Record *T : Records.getAllDerivedDefinitions("LegalizeNamedType"))
    CollectUsed(T->getValueAsString("Expr"));
  for (const Record *X : Records.getAllDerivedDefinitions("LegalizeInlineType"))
    CollectUsed(X->getValueAsString("Text"));
  for (const Record *X :
       Records.getAllDerivedDefinitions("LegalizeTypeListChoice"))
    CollectUsed(X->getValueAsString("Text"));
  for (const Record *X : Records.getAllDerivedDefinitions("LegalizeRawArg"))
    CollectUsed(X->getValueAsString("Text"));

  for (const Record *F : Records.getAllDerivedDefinitions("LegalizeFeature"))
    if (!UsedFeatures.contains(F->getName()))
      PrintWarning(F->getLoc(), "legalizer feature '" + F->getName() +
                                    "' is never used and is not emitted");

  emitFeatures(OS);
  emitTypes(OS);

  std::vector<const Record *> RuleSets =
      Records.getAllDerivedDefinitions("LegalizeRuleSet");
  if (RuleSets.empty())
    PrintFatalError("no LegalizeRuleSet records found");

  // getAllDerivedDefinitions() returns the records sorted by name.  Emit them
  // in source order instead, so that a generated fragment lines up with the
  // .td file it was written in.
  llvm::sort(RuleSets, LessRecordByID());

  // A duplicated opcode silently *appends* rules to the same rule set at
  // runtime, so this cannot be caught by the C++ compiler either.
  StringSet<> SeenOpcodes;

  for (const Record *RuleSet : RuleSets) {
    for (StringRef Opcode : RuleSet->getValueAsListOfStrings("Opcodes")) {
      if (!Opcode.starts_with("G_"))
        PrintFatalError(RuleSet->getLoc(),
                        "'" + Opcode + "' is not a generic opcode");
      if (!SeenOpcodes.insert(Opcode).second)
        PrintFatalError(RuleSet->getLoc(), "opcode '" + Opcode +
                                               "' is described by more than "
                                               "one rule set");
    }
    emitRuleSet(OS, *RuleSet);
  }
}

//===----------------------------------------------------------------------===//
// Core IR output (-gen-legalizer-core)
//
// The C++ fragment above is the product of the prototype.  This second output
// is the product of the specification work: a canonical JSON form that an
// independent evaluator can consume without reading any C++.  The expansion
// rules are the ones in LegalizerInfo.td; this code implements exactly that
// table and refuses anything the schema does not cover.
//===----------------------------------------------------------------------===//

namespace {

cl::opt<std::string> CoreTriple("core-triple",
                                cl::desc("triple recorded in the Core IR"),
                                cl::init("unspecified"));
cl::opt<std::string> CoreCPU("core-cpu",
                             cl::desc("cpu recorded in the Core IR"),
                             cl::init("unspecified"));
cl::opt<int> CorePointerWidth("core-pointer-width",
                              cl::desc("pointer width recorded in the Core IR"),
                              cl::init(64));

void jsonString(raw_ostream &OS, StringRef S) {
  OS << '"';
  for (char C : S) {
    if (C == '"' || C == '\\') {
      OS << '\\' << C;
      continue;
    }
    if (C == '\n') {
      OS << "\\n";
      continue;
    }
    if (static_cast<unsigned char>(C) >= 0x20)
      OS << C;
  }
  OS << '"';
}

/// Bit width of a literal LLT spelling.  Returns 0 for spellings we cannot
/// size statically (pointer without a width, inline expression).
unsigned lltBits(StringRef S) {
  StringRef T = S;
  unsigned Lanes = 1;
  if (T.consume_front("v")) {
    size_t L = 0;
    while (L < T.size() && isDigit(T[L]))
      ++L;
    if (L == 0)
      return 0;
    Lanes = std::stoul(T.substr(0, L).str());
    T = T.substr(L);
  }
  if (T.consume_front("p")) {
    size_t Colon = T.find(':');
    if (Colon == StringRef::npos)
      return 0;
    // A vector of pointers reaches here as "v2p0:64" with Lanes already read,
    // so its size is lanes * element bits, not element bits.
    return Lanes * std::stoul(T.substr(Colon + 1).str());
  }
  size_t I = 0;
  while (I < T.size() && !isDigit(T[I]))
    ++I;
  if (I == T.size())
    return 0;
  return Lanes * std::stoul(T.substr(I).str());
}

/// Number of lanes of a literal LLT spelling.  A scalar has one.
unsigned lltLanes(StringRef S) {
  StringRef T = S;
  if (!T.consume_front("v"))
    return 1;
  size_t L = 0;
  while (L < T.size() && isDigit(T[L]))
    ++L;
  return L == 0 ? 1 : std::stoul(T.substr(0, L).str());
}

/// Element spelling of a literal LLT: "v4s32" -> "s32", "s32" -> "s32".
std::string lltElt(StringRef S) {
  StringRef T = S;
  if (!T.consume_front("v"))
    return T.str();
  size_t L = 0;
  while (L < T.size() && isDigit(T[L]))
    ++L;
  return T.substr(L).str();
}

/// A JSON string literal, quotes included.
std::string jsonEscaped(StringRef S) {
  std::string Buf;
  raw_string_ostream OS(Buf);
  jsonString(OS, S);
  OS.flush();
  return Buf;
}

/// The records expandRule() is walking.  normalizeLlt() needs them only for
/// the one shape where a type names another type (LLT::fixed_vector(2, X86_p0)).
const RecordKeeper *CurrentRK = nullptr;

/// Lower one LegalizePredicate record to a Core IR predicate AST.  Declared
/// here because the type-expression expansion below needs the same feature
/// leaf spelling, and predicateJson() is defined further down.
void predicateJson(const RecordKeeper &Records, const Record &P,
                   std::string &Out);

const Record *namedTypeDef(StringRef Name) {
  if (!CurrentRK || Name.empty() || !isAlpha(Name.front()))
    return nullptr;
  if (Name.contains('(') || Name.contains(' ') || Name.contains(':'))
    return nullptr;
  const Record *R = CurrentRK->getDef(Name);
  if (!R || !R->isSubClassOf("LegalizeNamedType"))
    return nullptr;
  return R;
}

/// Two LLT constants in X86LegalizerInfo.td are spelled in terms of the
/// subtarget.  The corpus is generated for 64-bit X86 and the table only
/// installs itself there (see the is64Bit() guard in X86LegalizerInfo.cpp), so
/// pinning them is sound rather than approximate: the other instantiation
/// never meets this table.
StringRef pinSubtargetLlt(StringRef S) {
  if (S == "LLT::pointer(0, Subtarget.isTarget64BitLP64() ? 64 : 32)")
    return "LLT::pointer(0, 64)";
  if (S == "Subtarget.is64Bit() ? X86_s64 : X86_s32")
    return "LLT::scalar(64)";
  return S;
}

std::string normalizeLlt(StringRef S) {
  StringRef T = pinSubtargetLlt(S.trim());
  {
    StringRef R = T;
    if (R.consume_front("LLT::scalar(") && R.consume_back(")") && !R.empty() &&
        llvm::all_of(R, isDigit))
      return "s" + R.str();
  }
  {
    StringRef R = T;
    if (R.consume_front("LLT::fixed_vector(") && R.consume_back(")")) {
      size_t Comma = R.find(',');
      if (Comma != StringRef::npos) {
        StringRef L = R.substr(0, Comma).trim();
        StringRef E = R.substr(Comma + 1).trim();
        if (!L.empty() && !E.empty() && llvm::all_of(L, isDigit)) {
          if (llvm::all_of(E, isDigit))
            return "v" + L.str() + "s" + E.str();
          // The element is a named type, e.g. LLT::fixed_vector(2, X86_p0).
          if (const Record *D = namedTypeDef(E)) {
            std::string Inner = normalizeLlt(D->getValueAsString("Expr"));
            if (Inner.size() > 3 && Inner.front() == 'p' &&
                Inner.find(':') != std::string::npos)
              return "v" + L.str() + Inner;
            if (Inner.size() > 1 && Inner.front() == 's')
              return "v" + L.str() + Inner;
          }
        }
      }
    }
  }
  {
    StringRef R = T;
    if (R.consume_front("LLT::pointer(") && R.consume_back(")")) {
      size_t Comma = R.find(',');
      if (Comma != StringRef::npos) {
        StringRef AS = R.substr(0, Comma).trim();
        StringRef Bits = R.substr(Comma + 1).trim();
        if (!AS.empty() && !Bits.empty() && llvm::all_of(AS, isDigit) &&
            llvm::all_of(Bits, isDigit))
          return "p" + AS.str() + ":" + Bits.str();
      }
    }
  }
  return "";
}
/// Index of the first `C` outside any parentheses, or npos.
size_t topLevelIndex(StringRef S, char C) {
  int Depth = 0;
  for (size_t I = 0; I != S.size(); ++I) {
    if (S[I] == '(')
      ++Depth;
    else if (S[I] == ')')
      --Depth;
    else if (S[I] == C && Depth == 0)
      return I;
  }
  return StringRef::npos;
}

/// True if the first character's parenthesis closes on the last character.
bool wraps(StringRef S) {
  if (S.size() < 2 || S.front() != '(' || S.back() != ')')
    return false;
  int Depth = 0;
  for (size_t I = 0; I != S.size(); ++I) {
    if (S[I] == '(')
      ++Depth;
    else if (S[I] == ')') {
      --Depth;
      if (Depth == 0)
        return I == S.size() - 1;
    }
  }
  return false;
}

/// Lanes a normalised type names: "v8s64" -> 8, a scalar or a pointer -> 1.
bool lanesOfNormalized(StringRef N, unsigned &Lanes) {
  if (N.empty())
    return false;
  if (N.front() != 'v') {
    if (N.front() != 's' && N.front() != 'p')
      return false;
    Lanes = 1;
    return true;
  }
  size_t I = 1;
  while (I != N.size() && isDigit(N[I]))
    ++I;
  if (I == 1 || I == N.size())
    return false;
  Lanes = std::stoul(N.substr(1, I - 1).str());
  return true;
}

/// Lane count of a type expression, keeping the feature conditionals that pick
/// the type:  "HasAVX512 ? X86_v8s64 : HasAVX ? X86_v4s64 : X86_v2s64" becomes
/// "HasAVX512 ? 8 : (HasAVX ? 4 : 2)".  The count is a number; which number is
/// a subtarget question, and that question survives -- only the target's type
/// names go, because no consumer outside this file can resolve them.
bool lanesOfTypeExpr(StringRef E, std::string &Out) {
  E = E.trim();
  while (wraps(E))
    E = E.substr(1, E.size() - 2).trim();
  if (E.empty())
    return false;
  if (size_t Q = topLevelIndex(E, '?'); Q != StringRef::npos) {
    size_t Rest = topLevelIndex(E.substr(Q + 1), ':');
    if (Rest == StringRef::npos)
      return false;
    std::string Then, Else;
    if (!lanesOfTypeExpr(E.substr(Q + 1, Rest), Then) ||
        !lanesOfTypeExpr(E.substr(Q + 2 + Rest), Else))
      return false;
    // A conditional nested in the "then" arm needs its own parentheses: the
    // colon split below walks the text, and an unparenthesised inner colon
    // would steal the outer one.
    if (topLevelIndex(Then, '?') != StringRef::npos)
      Then = "(" + Then + ")";
    Out = E.substr(0, Q).trim().str() + " ? " + Then + " : (" + Else + ")";
    return true;
  }
  if (const Record *D = namedTypeDef(E)) {
    StringRef Inner = D->getValueAsString("Expr");
    if (Inner.trim() == E)
      return false;
    return lanesOfTypeExpr(Inner, Out);
  }
  unsigned Lanes = 0;
  if (!lanesOfNormalized(normalizeLlt(E), Lanes))
    return false;
  Out = std::to_string(Lanes);
  return true;
}

/// Replace every "<Name>.getNumElements()" in a lane bound with the lane count
/// <Name> denotes.  A lane count is the one argument a builder takes as a number
/// rather than a type, so a .td that wants "as many lanes as the widest s64
/// vector" had to spell C++; this resolves that spelling back into the bound
/// language the consumers of the core IR already know.
std::string resolveLaneBound(const Record &Rule, StringRef Bound) {
  static const char Suffix[] = ".getNumElements()";
  std::string Out = Bound.str();
  size_t Pos = 0;
  while ((Pos = Out.find(Suffix, Pos)) != std::string::npos) {
    size_t Begin = Pos;
    while (Begin > 0 && (isAlnum(Out[Begin - 1]) || Out[Begin - 1] == '_'))
      --Begin;
    std::string Name = Out.substr(Begin, Pos - Begin);
    std::string Lanes;
    if (Name.empty() || !lanesOfTypeExpr(Name, Lanes))
      PrintFatalError(Rule.getLoc(),
                      "cannot resolve '" + Name +
                          ".getNumElements()' to a lane count: name a type "
                          "that has one, or spell the count directly");
    // The replacement is spliced into whatever expression the .td wrote, so
    // a conditional inside it needs its own parentheses: the bound is read
    // left to right and an unparenthesised inner colon would steal the outer
    // conditional's split.
    if (Lanes.find('?') != std::string::npos)
      Lanes = "(" + Lanes + ")";
    Out.replace(Begin, Pos + strlen(Suffix) - Begin, Lanes);
    Pos = Begin + Lanes.size();
  }
  return Out;
}

std::string typeIs(unsigned Idx, StringRef Ty) {
  return "{\"kind\":\"leaf\",\"name\":\"typeIs\",\"args\":[" +
         std::to_string(Idx) + ",\"" + Ty.str() + "\"]}";
}

/// memSize(mem_idx, size_bits, min_align_bits): the MMO at mem_idx has the
/// given size and at least the given alignment.  This mirrors
/// LegalityPredicates::typePairAndMemDescInSet, which compares the type pair
/// exactly, the memory type by size, and the alignment with >= (see
/// TypePairAndMemDesc::isCompatible).
std::string memSize(unsigned Idx, unsigned SizeBits, unsigned MinAlignBits) {
  return "{\"kind\":\"leaf\",\"name\":\"memSize\",\"args\":[" +
         std::to_string(Idx) + "," + std::to_string(SizeBits) + "," +
         std::to_string(MinAlignBits) + "]}";
}

std::string allOf(const std::vector<std::string> &Parts) {
  std::string S = "{\"kind\":\"nary\",\"op\":\"all_of\",\"args\":[";
  for (size_t I = 0; I != Parts.size(); ++I) {
    if (I)
      S += ",";
    S += Parts[I];
  }
  return S + "]}";
}

/// sizeIs(idx, bits): the slot's total size, not its scalar size.  A v4s32 and
/// an s128 are both 128 here, which is the point: the merge/unmerge rules ask
/// whether the whole value fits a register, and its shape is irrelevant.
std::string sizeIs(unsigned Idx, StringRef Bits) {
  return "{\"kind\":\"leaf\",\"name\":\"sizeIs\",\"args\":[" +
         std::to_string(Idx) + ",\"" + Bits.str() + "\"]}";
}

std::string scalarNarrowerThan(unsigned Idx, StringRef Bits) {
  return "{\"kind\":\"leaf\",\"name\":\"scalarNarrowerThan\",\"args\":[" +
         std::to_string(Idx) + ",\"" + Bits.str() + "\"]}";
}

std::string scalarNarrowerThan(unsigned Idx, unsigned Bits) {
  return scalarNarrowerThan(Idx, std::to_string(Bits));
}

std::string scalarWiderThan(unsigned Idx, StringRef Bits) {
  return "{\"kind\":\"leaf\",\"name\":\"scalarWiderThan\",\"args\":[" +
         std::to_string(Idx) + ",\"" + Bits.str() + "\"]}";
}

std::string scalarWiderThan(unsigned Idx, unsigned Bits) {
  return scalarWiderThan(Idx, std::to_string(Bits));
}

std::string oneArgLeaf(StringRef Name, unsigned A) {
  return "{\"kind\":\"leaf\",\"name\":\"" + Name.str() +
         "\",\"args\":[" + std::to_string(A) + "]}";
}

std::string isVector(unsigned Idx) { return oneArgLeaf("isVector", Idx); }

std::string sizeNotPow2(unsigned Idx) {
  return oneArgLeaf("sizeNotPow2", Idx);
}

std::string scalarOrEltSizeNotPow2(unsigned Idx) {
  return oneArgLeaf("scalarOrEltSizeNotPow2", Idx);
}

std::string numElementsNotPow2(unsigned Idx) {
  return oneArgLeaf("numElementsNotPow2", Idx);
}

std::string fewerElementsThan(unsigned Idx, unsigned Lanes) {
  return "{\"kind\":\"leaf\",\"name\":\"fewerElementsThan\",\"args\":[" +
         std::to_string(Idx) + "," + std::to_string(Lanes) + "]}";
}

std::string moreElementsThan(unsigned Idx, unsigned Lanes) {
  return "{\"kind\":\"leaf\",\"name\":\"moreElementsThan\",\"args\":[" +
         std::to_string(Idx) + "," + std::to_string(Lanes) + "]}";
}

std::string elementTypeIs(unsigned Idx, StringRef Ty) {
  return "{\"kind\":\"leaf\",\"name\":\"elementTypeIs\",\"args\":[" +
         std::to_string(Idx) + ",\"" + Ty.str() + "\"]}";
}

/// One literal of a guard: a subtarget condition, possibly negated.  A guard is
/// a conjunction of literals, so it is kept in that form until it is written
/// out; spelling it as JSON early would make the disjunction merge below
/// unable to see that `Cond` and `!Cond` cancel.
struct GuardLit {
  std::string Text;
  bool Neg = false;
};

using Guard = std::vector<GuardLit>;

/// One branch of a type expression: the literal type and the feature
/// condition under which that branch is taken.  An empty guard means the
/// branch is unconditional.
struct TypeAlt {
  Guard G;
  std::string Type; // normalised LLT spelling
};

/// One branch of a type list, with the same guard convention.
struct ListAlt {
  Guard G;
  std::vector<std::string> Types;
};

/// Rewrite the C++ spelling of a feature condition into the text the Core IR
/// carries.  A LegalizeFeature record name (`HasAVX`) becomes its `Cond`
/// (`Subtarget.hasAVX()`); everything else -- a call, an operator, a
/// parenthesis -- is copied.  The walk is over whole identifiers, so `HasAVX`
/// cannot rewrite the prefix of `HasAVX512`.
std::string conditionText(const RecordKeeper &Records, StringRef Cond) {
  std::string Out;
  size_t I = 0;
  while (I != Cond.size()) {
    if (!(isAlpha(Cond[I]) || Cond[I] == '_')) {
      Out += Cond[I++];
      continue;
    }
    size_t Begin = I;
    while (I != Cond.size() && (isAlnum(Cond[I]) || Cond[I] == '_'))
      ++I;
    StringRef Id = Cond.slice(Begin, I);
    if (const Record *F = Records.getDef(Id))
      if (F->isSubClassOf("LegalizeFeature")) {
        Out += F->getValueAsString("Cond").str();
        continue;
      }
    Out += Id.str();
  }
  return Out;
}

std::string featureLeaf(StringRef Text) {
  return "{\"kind\":\"leaf\",\"name\":\"hasFeature\",\"args\":[" +
         jsonEscaped(Text) + "]}";
}

std::string negatePredicate(const std::string &P) {
  return "{\"kind\":\"not\",\"arg\":" + P + "}";
}

Guard featureGuard(const RecordKeeper &Records, StringRef Cond) {
  Guard G;
  G.push_back({conditionText(Records, Cond.trim()), false});
  return G;
}

Guard negatedGuard(Guard G) {
  for (GuardLit &L : G)
    L.Neg = !L.Neg;
  return G;
}

/// Conjunction.  An identical literal is not repeated; opposite literals are
/// both kept, which makes the guard unsatisfiable, exactly as it should be.
Guard conjoin(const Guard &A, const Guard &B) {
  Guard Out = A;
  for (const GuardLit &L : B) {
    bool Seen = llvm::any_of(Out, [&](const GuardLit &E) {
      return E.Text == L.Text && E.Neg == L.Neg;
    });
    if (!Seen)
      Out.push_back(L);
  }
  return Out;
}

/// The Core IR spelling of a conjunction of literals; empty means "true", so
/// callers can splice it into a rule predicate without a guard wrapper.
std::string guardJson(const Guard &G) {
  std::vector<std::string> Parts;
  for (const GuardLit &L : G) {
    std::string Leaf = featureLeaf(L.Text);
    Parts.push_back(L.Neg ? negatePredicate(Leaf) : Leaf);
  }
  if (Parts.empty())
    return "";
  if (Parts.size() == 1)
    return Parts.front();
  return allOf(Parts);
}

/// \p Guard AND \p Pred, as a predicate AST.
std::string guardAnd(const Guard &G, const std::string &Pred) {
  std::string GJ = guardJson(G);
  if (GJ.empty())
    return Pred;
  return allOf({GJ, Pred});
}

/// The same conjunction when both sides are already predicate ASTs; an empty
/// side is "true".
std::string joinPredicate(const std::string &A, const std::string &B) {
  if (A.empty())
    return B;
  if (B.empty())
    return A;
  return allOf({A, B});
}

/// Split `Cond ? Then : Else` at the top level.  A conditional nested in the
/// "then" arm has to be parenthesised: this walk cannot tell its colon from
/// the outer one, so it is refused rather than guessed.  Nesting in the "else"
/// arm is right-associative and handled by the recursive caller.
bool splitTypeSelect(StringRef Expr, StringRef &Cond, StringRef &Then,
                     StringRef &Else) {
  Expr = Expr.trim();
  while (wraps(Expr))
    Expr = Expr.substr(1, Expr.size() - 2).trim();
  size_t Q = topLevelIndex(Expr, '?');
  if (Q == StringRef::npos)
    return false;
  size_t C = topLevelIndex(Expr.substr(Q + 1), ':');
  if (C == StringRef::npos)
    return false;
  Cond = Expr.substr(0, Q).trim();
  Then = Expr.substr(Q + 1, C).trim();
  Else = Expr.substr(Q + 1 + C + 1).trim();
  if (Cond.empty() || Then.empty() || Else.empty())
    return false;
  return topLevelIndex(Then, '?') == StringRef::npos;
}

/// Expand one type expression into literal alternatives, one per branch of its
/// feature conditionals.  \p G carries the conditions already taken.
///
/// A bound like `HasSSE2 ? X86_s64 : X86_s32` becomes two alternatives; the
/// residual rule form is `(Cond ? A : B) == (Cond,A) | (!Cond,B)`, and both
/// branches are literal, which is what the Core IR can carry.
bool expandTypeExpr(const RecordKeeper &Records, StringRef Expr, const Guard &G,
                    std::vector<TypeAlt> &Out, std::string &Why) {
  StringRef E = Expr.trim();
  while (wraps(E))
    E = E.substr(1, E.size() - 2).trim();
  if (E.empty()) {
    Why = "nonliteral_type";
    return false;
  }
  std::string Norm = normalizeLlt(E);
  if (!Norm.empty()) {
    Out.push_back({G, Norm});
    return true;
  }
  if (const Record *D = namedTypeDef(E)) {
    StringRef Inner = D->getValueAsString("Expr");
    if (Inner.trim() == E) {
      Why = "nonliteral_type";
      return false;
    }
    return expandTypeExpr(Records, Inner, G, Out, Why);
  }
  StringRef Cond, Then, Else;
  if (!splitTypeSelect(E, Cond, Then, Else)) {
    Why = "nonliteral_type";
    return false;
  }
  Guard CG = featureGuard(Records, Cond);
  if (!expandTypeExpr(Records, Then, conjoin(G, CG), Out, Why))
    return false;
  return expandTypeExpr(Records, Else, conjoin(G, negatedGuard(CG)), Out, Why);
}

/// Alternatives of one type argument: a named type (which may itself be
/// spelled as a conditional), or an inline expression.
bool typeAlternatives(const RecordKeeper &Records, const Record &A,
                      std::vector<TypeAlt> &Out, std::string &Why) {
  if (A.isSubClassOf("LegalizeNamedType"))
    return expandTypeExpr(Records, A.getValueAsString("Expr"), {}, Out, Why);
  if (A.isSubClassOf("LegalizeInlineType"))
    return expandTypeExpr(Records, A.getValueAsString("Text"), {}, Out, Why);
  Why = "nonliteral_type";
  return false;
}

/// Alternatives of one type-list argument.  A list is the cartesian product of
/// its elements' alternatives; a choice between two named lists contributes
/// both arms, each under its condition.
bool listAlternatives(const RecordKeeper &Records, const Record &L,
                      std::vector<ListAlt> &Out, std::string &Why) {
  if (L.isSubClassOf("LegalizeNamedTypeList") ||
      L.isSubClassOf("LegalizeInlineTypeList")) {
    std::vector<ListAlt> Acc{{Guard{}, {}}};
    for (const Record *E : L.getValueAsListOfDefs("Elems")) {
      std::vector<TypeAlt> Alts;
      if (!expandTypeExpr(Records, E->getValueAsString("Expr"), {}, Alts, Why))
        return false;
      std::vector<ListAlt> Next;
      for (const ListAlt &Prefix : Acc)
        for (const TypeAlt &A : Alts) {
          ListAlt N;
          N.G = conjoin(Prefix.G, A.G);
          N.Types = Prefix.Types;
          N.Types.push_back(A.Type);
          Next.push_back(std::move(N));
        }
      Acc = std::move(Next);
    }
    for (ListAlt &A : Acc)
      Out.push_back(std::move(A));
    return true;
  }
  if (L.isSubClassOf("LegalizeTypeListChoice")) {
    StringRef Cond, Then, Else;
    if (!splitTypeSelect(L.getValueAsString("Text"), Cond, Then, Else)) {
      Why = "type_list_choice";
      return false;
    }
    Guard CG = featureGuard(Records, Cond);
    auto Arm = [&](StringRef Name, const Guard &G) -> bool {
      const Record *R = Records.getDef(Name);
      if (!R || !(R->isSubClassOf("LegalizeNamedTypeList") ||
                  R->isSubClassOf("LegalizeInlineTypeList"))) {
        Why = "type_list_choice";
        return false;
      }
      std::vector<ListAlt> Alts;
      if (!listAlternatives(Records, *R, Alts, Why))
        return false;
      for (ListAlt &A : Alts) {
        A.G = conjoin(G, A.G);
        Out.push_back(std::move(A));
      }
      return true;
    };
    return Arm(Then, CG) && Arm(Else, negatedGuard(CG));
  }
  Why = "nonliteral_type";
  return false;
}

bool sameLiteral(const GuardLit &A, const GuardLit &B) {
  return A.Text == B.Text && A.Neg == B.Neg;
}

bool sameClause(const Guard &A, const Guard &B) {
  if (A.size() != B.size())
    return false;
  return llvm::all_of(A, [&](const GuardLit &L) {
    return llvm::any_of(B,
                        [&](const GuardLit &E) { return sameLiteral(L, E); });
  });
}

/// Disjunction of conjunctions, reduced by resolution.  The case that matters
/// is a type both arms of a choice name: its guard arrives as
/// `Cond` and `!Cond`, and the resolvent is the (possibly empty) shared
/// condition.  Without this the rule would carry `any_of(Cond, !Cond)`, which
/// is true but not recognisable as unconditional, and would make the Core IR
/// ask a question the description never asked.
void simplifyDisjunction(std::vector<Guard> &Clauses) {
  bool Changed = true;
  while (Changed) {
    Changed = false;
    for (size_t I = 0; I != Clauses.size() && !Changed; ++I)
      for (size_t J = I + 1; J != Clauses.size() && !Changed; ++J) {
        if (sameClause(Clauses[I], Clauses[J])) {
          Clauses.erase(Clauses.begin() + J);
          Changed = true;
          break;
        }
        Guard OnlyI, OnlyJ;
        for (const GuardLit &L : Clauses[I])
          if (!llvm::any_of(Clauses[J], [&](const GuardLit &E) {
                return sameLiteral(L, E);
              }))
            OnlyI.push_back(L);
        for (const GuardLit &L : Clauses[J])
          if (!llvm::any_of(Clauses[I], [&](const GuardLit &E) {
                return sameLiteral(L, E);
              }))
            OnlyJ.push_back(L);
        // I subsumes J: J says no more than I, so it adds nothing.
        if (OnlyJ.empty()) {
          Clauses.erase(Clauses.begin() + J);
          Changed = true;
          break;
        }
        Guard Resolvent;
        for (const GuardLit &L : Clauses[I])
          if (!llvm::any_of(
                  OnlyI, [&](const GuardLit &E) { return sameLiteral(L, E); }))
            Resolvent.push_back(L);
        if (OnlyI.size() == 1 && OnlyJ.size() == 1 &&
            OnlyI[0].Text == OnlyJ[0].Text && OnlyI[0].Neg != OnlyJ[0].Neg) {
          Clauses[I] = std::move(Resolvent);
          Clauses.erase(Clauses.begin() + J);
          Changed = true;
          break;
        }
      }
  }
}

/// Collapse list alternatives into one guard per type.  A type both arms name
/// resolves to unconditional; a type only one arm names keeps that arm's
/// condition.  First-appearance order is kept so rule order stays stable.
std::vector<std::pair<std::string, std::string>>
mergeTypeGuards(const std::vector<ListAlt> &Alts) {
  std::vector<std::pair<Guard, std::string>> Out;
  std::vector<std::vector<Guard>> Disj; // one disjunction per output entry
  for (const ListAlt &L : Alts)
    for (const std::string &T : L.Types) {
      size_t Pos = 0;
      while (Pos != Out.size() && Out[Pos].second != T)
        ++Pos;
      if (Pos == Out.size()) {
        Out.push_back({L.G, T});
        Disj.push_back({L.G});
        continue;
      }
      bool Seen = llvm::any_of(
          Disj[Pos], [&](const Guard &C) { return sameClause(C, L.G); });
      if (!Seen)
        Disj[Pos].push_back(L.G);
    }
  std::vector<std::pair<std::string, std::string>> Rendered;
  for (size_t I = 0; I != Out.size(); ++I) {
    simplifyDisjunction(Disj[I]);
    std::string Json;
    if (Disj[I].size() == 1) {
      Json = guardJson(Disj[I].front());
    } else {
      // More than one clause survives: the type is legal under any of them.
      Json = "{\"kind\":\"nary\",\"op\":\"any_of\",\"args\":[";
      for (size_t K = 0; K != Disj[I].size(); ++K) {
        if (K)
          Json += ",";
        std::string Clause = guardJson(Disj[I][K]);
        Json += Clause.empty()
                    ? "{\"kind\":\"leaf\",\"name\":\"true\",\"args\":[]}"
                    : Clause;
      }
      Json += "]}";
    }
    Rendered.push_back({std::move(Json), Out[I].second});
  }
  return Rendered;
}

/// The same two relations, with the lane count spelled as a C++ expression.
///
/// clampMinNumElements takes its bound as `unsigned`, but X86 writes
/// "HasBWI ? 64 : (HasAVX2 ? 32 : 16)" because the bound depends on the
/// subtarget.  That is data, not a type: it travels as a string and is
/// resolved once per configuration, the contract sizeIs already has.  Guessing
/// a number here would have put a wrong lane count in the corpus and no check
/// would have gone red.
std::string fewerElementsThanExpr(unsigned Idx, StringRef Bound) {
  return "{\"kind\":\"leaf\",\"name\":\"fewerElementsThan\",\"args\":[" +
         std::to_string(Idx) + ",\"" + Bound.str() + "\"]}";
}

std::string moreElementsThanExpr(unsigned Idx, StringRef Bound) {
  return "{\"kind\":\"leaf\",\"name\":\"moreElementsThan\",\"args\":[" +
         std::to_string(Idx) + ",\"" + Bound.str() + "\"]}";
}

/// Types[Other].getScalarSizeInBits() > Types[Idx].getSizeInBits()
std::string scalarSizeLargerThanSlot(unsigned Other, unsigned Idx) {
  return "{\"kind\":\"leaf\",\"name\":\"scalarSizeLargerThanSlot\","
         "\"args\":[" + std::to_string(Other) + "," + std::to_string(Idx) +
         "]}";
}

/// Types[Other].getScalarSizeInBits() < Types[Idx].getSizeInBits()
std::string scalarSizeSmallerThanSlot(unsigned Other, unsigned Idx) {
  return "{\"kind\":\"leaf\",\"name\":\"scalarSizeSmallerThanSlot\","
         "\"args\":[" + std::to_string(Other) + "," + std::to_string(Idx) +
         "]}";
}

/// A LegalizeTypePredicate carries a C++ call such as `typeIs(0, X86_s32)`.
/// Its Core IR form is the same leaf vocabulary the expansion uses, so the call
/// has to be translated rather than passed through: the Core IR names types in
/// the normalized `s32` / `v4s32` form, not by the target's record names.
///
/// Only the shapes the evaluator already understands are accepted.  Anything
/// else is an error rather than an opaque leaf, because a rule that reaches
/// this point has claimed to be expressible.
std::string typePredicateJson(const RecordKeeper &Records,
                              const Record &R) {
  StringRef Call = R.getValueAsString("Call").trim();

  auto SplitArgs = [&](StringRef Body, SmallVectorImpl<StringRef> &Out) {
    Body.split(Out, ',');
    for (StringRef &A : Out)
      A = A.trim();
  };

  StringRef Rest = Call;
  StringRef Name = Rest.take_until([](char C) { return C == '('; });
  if (!Rest.consume_front(Name) || !Rest.consume_front("(") ||
      !Rest.consume_back(")"))
    PrintFatalError(R.getLoc(),
                    "type predicate '" + Call + "' is not a call");

  // typeInSet's second argument is a braced set, so it cannot go through the
  // comma split below: the commas inside the braces are element separators.
  if (Name == "typeInSet") {
    size_t Comma = Rest.find(',');
    if (Comma == StringRef::npos)
      PrintFatalError(R.getLoc(), "typeInSet needs (idx, {types}): " + Call);
    unsigned Idx0 = 0;
    if (Rest.substr(0, Comma).trim().getAsInteger(10, Idx0))
      PrintFatalError(R.getLoc(), "typeInSet index is not a number: " + Call);
    StringRef Body = Rest.substr(Comma + 1).trim();
    if (!Body.consume_front("{"))
      PrintFatalError(R.getLoc(), "typeInSet needs a braced set: " + Call);
    size_t Close = Body.rfind("}");
    if (Close == StringRef::npos)
      PrintFatalError(R.getLoc(),
                      "typeInSet is missing its closing brace: " + Call);
    SmallVector<StringRef, 8> Elems;
    Body.substr(0, Close).split(Elems, ',');
    std::string Out = "{\"kind\":\"leaf\",\"name\":\"typeIn\","
                      "\"args\":[" + std::to_string(Idx0) + ",[";
    StringRef Sep;
    for (StringRef E : Elems) {
      E = E.trim();
      if (E.empty())
        continue;
      std::string Norm = normalizeLlt(E);
      if (Norm.empty()) {
        const Record *T = Records.getDef(E);
        if (T && T->isSubClassOf("LegalizeNamedType"))
          Norm = normalizeLlt(T->getValueAsString("Expr"));
      }
      if (Norm.empty())
        PrintFatalError(R.getLoc(), "typeInSet names a type with no literal "
                                        "Core IR form: " + E);
      Out += Sep.str();
      Out += "\"" + Norm + "\"";
      Sep = ",";
    }
    return Out + "]]}";
  }

  SmallVector<StringRef, 4> Args;
  SplitArgs(Rest, Args);

  auto Idx = [&](size_t I) {
    unsigned V = 0;
    if (I >= Args.size() || Args[I].getAsInteger(10, V))
      PrintFatalError(R.getLoc(),
                      "type predicate '" + Call + "' needs an index at " +
                          std::to_string(I));
    return V;
  };
  // A type argument is either a normalized LLT or the name of a
  // LegalizeNamedType record; the Core IR only knows the former, so a record
  // name is resolved through the record it names.  Resolving rather than
  // rejecting is what keeps the two spellings from being able to disagree: the
  // C++ this emits uses the record, and the Core IR has to see the same type.
  auto Ty = [&](size_t I) {
    if (I >= Args.size())
      PrintFatalError(R.getLoc(),
                      "type predicate '" + Call + "' is missing a type");
    std::string Norm = normalizeLlt(Args[I]);
    if (!Norm.empty())
      return Norm;
    if (const Record *T = Records.getDef(Args[I])) {
      if (T->isSubClassOf("LegalizeNamedType")) {
        std::string FromRecord =
            normalizeLlt(T->getValueAsString("Expr"));
        if (!FromRecord.empty())
          return FromRecord;
      }
    }
    PrintFatalError(R.getLoc(),
                    "type predicate '" + Call + "' names a type that has no "
                    "literal Core IR form: " + Args[I]);
    return std::string();
  };

  // A bit width may be a compile-time expression (`Is64Bit ? 64 : 32`).  The
  // Core IR keeps it as text in that case rather than guessing a number: the
  // evaluator compares it symbolically against the configuration.
  auto Bits = [&](size_t I) {
    if (I >= Args.size())
      PrintFatalError(R.getLoc(),
                      "type predicate '" + Call + "' is missing a width");
    unsigned V = 0;
    if (!Args[I].getAsInteger(10, V))
      return std::to_string(V);
    // A width expression may name a feature record (Is64Bit) rather than spell
    // the condition out.  The Core IR records conditions, not record names, so
    // the name is resolved here; otherwise the evaluator would see an
    // identifier it has no configuration entry for.
    SmallVector<StringRef, 4> Idents;
    collectIdentifiers(Args[I], Idents);
    std::string Text = Args[I].str();
    for (StringRef Id : Idents) {
      const Record *F = Records.getDef(Id);
      if (!F || !F->isSubClassOf("LegalizeFeature"))
        continue;
      StringRef Cond = F->getValueAsString("Cond");
      size_t Pos = Text.find(Id.str());
      if (Pos != std::string::npos)
        Text = Text.substr(0, Pos) + Cond.str() + Text.substr(Pos + Id.size());
    }
    return Text;
  };

  if (Name == "typeIs")
    return typeIs(Idx(0), Ty(1));
  if (Name == "isVector")
    return isVector(Idx(0));
  if (Name == "scalarNarrowerThan")
    return scalarNarrowerThan(Idx(0), Bits(1));
  if (Name == "scalarWiderThan")
    return scalarWiderThan(Idx(0), Bits(1));
  if (Name == "sizeIs")
    return sizeIs(Idx(0), Bits(1));
  PrintFatalError(R.getLoc(),
                  "type predicate '" + Name + "' has no Core IR expansion; "
                  "add it to LegalizerInfo.td first");
  return "";
}

/// Lower one LegalizePredicate record to a Core IR predicate AST.
void predicateJson(const RecordKeeper &Records, const Record &P,
                   std::string &Out) {
  if (P.isSubClassOf("LegalizeFeature")) {
    Out = "{\"kind\":\"leaf\",\"name\":\"hasFeature\",\"args\":[\"" +
          P.getValueAsString("Cond").str() + "\"]}";
    return;
  }
  if (P.isSubClassOf("LegalizeBoolPredicateDag") ||
      P.isSubClassOf("LegalizeQueryPredicateDag")) {
    const DagInit *D = P.getValueAsDag("CondDag");
    std::function<std::string(const Init &)> Walk =
        [&](const Init &Node) -> std::string {
      if (const auto *DI = dyn_cast<DefInit>(&Node)) {
        const Record *R = DI->getDef();
        if (R->isSubClassOf("LegalizeFeature"))
          return "{\"kind\":\"leaf\",\"name\":\"hasFeature\",\"args\":[\"" +
                 R->getValueAsString("Cond").str() + "\"]}";
        if (R->isSubClassOf("LegalizeTypePredicate"))
          return typePredicateJson(Records, *R);
        PrintFatalError(R->getLoc(), "predicate dag leaf '" + R->getName() +
                                         "' is not a LegalizeFeature or a "
                                         "LegalizeTypePredicate");
      }
      const auto *N = dyn_cast<DagInit>(&Node);
      if (!N)
        PrintFatalError(P.getLoc(), "unsupported predicate dag node");
      std::string Op = N->getOperator()->getAsString();
      std::vector<std::string> Kids;
      for (const Init *Arg : N->getArgs())
        Kids.push_back(Walk(*Arg));
      if (Op == "not")
        return "{\"kind\":\"not\",\"arg\":" + Kids.front() + "}";
      std::string OpName = Op == "and" ? "all_of" : Op == "or" ? "any_of" : Op;
      std::string S = "{\"kind\":\"nary\",\"op\":\"" + OpName + "\",\"args\":[";
      for (size_t I = 0; I != Kids.size(); ++I) {
        if (I)
          S += ",";
        S += Kids[I];
      }
      return S + "]}";
    };
    Out = Walk(*D);
    return;
  }  if (P.isSubClassOf("LegalizeOpaqueBool") ||
      P.isSubClassOf("LegalizeQueryPredicate")) {
    // Escape: raw C++.  It has no Core IR form by construction.
    Out = "{\"kind\":\"leaf\",\"name\":\"opaque\",\"args\":[\"" +
          P.getName().str() + "\"]}";
    return;
  }
  PrintFatalError(&P, "unknown LegalizePredicate record");
}

/// Flatten a type argument into literal LLT spellings.  An inline expression
/// has no literal form, so it is an error rather than a silent loss.
void collectTypes(const Record &R, std::vector<std::string> &Out) {
  if (R.isSubClassOf("LegalizeNamedType")) {
    Out.push_back(normalizeLlt(R.getValueAsString("Expr")));
    return;
  }
  if (R.isSubClassOf("LegalizeNamedTypeList") ||
      R.isSubClassOf("LegalizeInlineTypeList")) {
    for (const Record *E : R.getValueAsListOfDefs("Elems"))
      Out.push_back(normalizeLlt(E->getValueAsString("Expr")));
    return;
  }
  if (R.isSubClassOf("LegalizeInlineType")) {
    PrintFatalError(R.getLoc(),
                    "inline type expression '" + R.getValueAsString("Text") +
                        "' has no literal Core IR form");
  }
  PrintFatalError(R.getLoc(), "unexpected type argument '" + R.getName() + "'");
}

SMLoc firstLoc(const Record &R) {
  ArrayRef<SMLoc> Locs = R.getLoc();
  return Locs.empty() ? SMLoc() : Locs.front();
}

unsigned originLine(const Record &R) {
  SMLoc L = firstLoc(R);
  return L.getPointer() ? SrcMgr.getLineAndColumn(L).first : 0;
}

StringRef originFile(const Record &R) {
  SMLoc L = firstLoc(R);
  if (!L.getPointer())
    return "<unknown>";
  return SrcMgr.getMemoryBuffer(SrcMgr.FindBufferContainingLoc(L))
      ->getBufferIdentifier();
}

struct CoreRule {
  std::string Action;
  unsigned TypeIdx = 0;
  std::string Predicate = "{\"kind\":\"leaf\",\"name\":\"true\",\"args\":[]}";
  std::string Mutation;  // empty means null
};

std::string mutationJson(unsigned Target, StringRef Kind, StringRef Value) {
  return "{\"target\":" + std::to_string(Target) + ",\"kind\":\"" + Kind.str() +
         "\",\"value\":" + (Value.empty() ? std::string("null")
                                          : "\"" + Value.str() + "\"") +
         "}";
}

std::string mutationJsonInt(unsigned Target, StringRef Kind, int Value) {
  return "{\"target\":" + std::to_string(Target) + ",\"kind\":\"" + Kind.str() +
         "\",\"value\":" + std::to_string(Value) + "}";
}

/// A lane-count bound is emitted as an integer when it is one, so the common
/// case stays readable and diffable; only the configured ones stay strings.
std::string mutationJsonLanes(unsigned Target, StringRef Bound) {
  bool Digits =
      !Bound.empty() && Bound.find_first_not_of("0123456789") == StringRef::npos;
  if (Digits)
    return mutationJsonInt(Target, "set_lanes", std::stoi(Bound.str()));
  return mutationJson(Target, "set_lanes", Bound);
}

/// Expand one LegalizeRule record into zero or more core rules.
void expandRule(const RecordKeeper &Records, const Record &Rule,
                std::vector<CoreRule> &Out, std::string &Why) {
  Why.clear();
  // A rule whose body is raw C++ is a judgment escape.  It produces no core
  // rule; runCore() records it in escapes[] so the count stays visible.
  if (!Rule.getValueAsString("OpaqueArgs").empty()) {
    Why = "opaque_rule";
    return;
  }

  StringRef Method = builderName(Rule);
  StringRef Builder = builderRecordName(Rule);
  std::vector<const Record *> Args = Rule.getValueAsListOfDefs("Args");

  // Arguments whose value only exists at code-generation time cannot be
  // expanded into a literal Core IR rule.  Naming them here keeps the escape
  // visible instead of guessing a type.
  //
  // The builders below are the exception: their type arguments are bounds, and
  // a bound spelled as a feature conditional is residualized into one rule per
  // branch with a hasFeature guard, so it does have a literal Core IR form.
  // Every other builder still escapes on a non-literal argument -- guessing
  // there would put an answer in a cell the key cannot justify.
  const bool ExprBuilder =
      Builder == "BuilderClampScalar" || Builder == "BuilderClampScalarOrElt" ||
      Builder == "BuilderMinScalar" || Builder == "BuilderMaxScalar" ||
      Builder == "BuilderClampNumElements" || Builder == "BuilderLegalFor" ||
      Builder == "BuilderLegalForCartesianProduct";

  auto CheckType = [&](const Record &T) -> bool {
    std::vector<TypeAlt> Alts;
    std::string Inner;
    if (typeAlternatives(Records, T, Alts, Inner))
      return true;
    Why = Inner;
    return false;
  };
  auto CheckList = [&](const Record &L) -> bool {
    std::vector<ListAlt> Alts;
    std::string Inner;
    if (listAlternatives(Records, L, Alts, Inner))
      return true;
    Why = Inner;
    return false;
  };

  for (const Record *A : Args) {
    if (A->isSubClassOf("LegalizeTypePair")) {
      const Record *T0 = A->getValueAsDef("Type0");
      const Record *T1 = A->getValueAsDef("Type1");
      if (ExprBuilder && Builder == "BuilderLegalFor") {
        if (!CheckType(*T0) || !CheckType(*T1))
          return;
        continue;
      }
      if (normalizeLlt(T0->getValueAsString("Expr")).empty() ||
          normalizeLlt(T1->getValueAsString("Expr")).empty()) {
        Why = "nonliteral_type";
        return;
      }
      continue;
    }
    if (A->isSubClassOf("LegalizeMemDesc")) {
      if (normalizeLlt(A->getValueAsDef("Type0")->getValueAsString("Expr")).empty() ||
          normalizeLlt(A->getValueAsDef("Type1")->getValueAsString("Expr")).empty() ||
          normalizeLlt(A->getValueAsDef("MemTy")->getValueAsString("Expr")).empty()) {
        Why = "nonliteral_type";
        return;
      }
      continue;
    }
    if (A->isSubClassOf("LegalizeNamedType")) {
      if (ExprBuilder) {
        if (!CheckType(*A))
          return;
      } else if (normalizeLlt(A->getValueAsString("Expr")).empty()) {
        Why = "nonliteral_type";
        return;
      }
      continue;
    }
    if (A->isSubClassOf("LegalizeNamedTypeList") ||
        A->isSubClassOf("LegalizeInlineTypeList")) {
      if (ExprBuilder) {
        if (!CheckList(*A))
          return;
      } else {
        for (const Record *E : A->getValueAsListOfDefs("Elems"))
          if (normalizeLlt(E->getValueAsString("Expr")).empty()) {
            Why = "nonliteral_type";
            return;
          }
      }
      continue;
    }
    if (A->isSubClassOf("LegalizeArgInt"))
      continue;
    if (A->isSubClassOf("LegalizeTypeListChoice")) {
      if (ExprBuilder) {
        if (!CheckList(*A))
          return;
        continue;
      }
      Why = "type_list_choice";
      return;
    }
    if (A->isSubClassOf("LegalizeInlineType")) {
      if (ExprBuilder) {
        if (!CheckType(*A))
          return;
        continue;
      }
      Why = "inline_type_expr";
      return;
    }
    if (A->isSubClassOf("LegalizeRawArg")) {
      // clampMin/MaxNumElements carry the lane count, not a type.  A count
      // that depends on the subtarget is resolvable by the consumer, so it is
      // not an escape; every other raw argument still is.
      if (Builder == "BuilderClampMinNumElements" ||
          Builder == "BuilderClampMaxNumElements")
        continue;
      Why = "raw_argument";
      return;
    }
  }

  std::string Pred;
  if (const Record *P = Rule.getValueAsOptionalDef("Pred"))
    predicateJson(Records, *P, Pred);

  auto WithPred = [&](std::string Cond) -> std::string {
    if (Pred.empty() && Cond.empty())
      return "{\"kind\":\"leaf\",\"name\":\"true\",\"args\":[]}";
    if (Pred.empty())
      return Cond;
    if (Cond.empty())
      return Pred;
    return allOf({Pred, Cond});
  };

  auto Int = [&](size_t I) {
    if (I >= Args.size() || !Args[I]->isSubClassOf("LegalizeArgInt"))
      PrintFatalError(Rule.getLoc(), "rule '" + Method + "' expects an integer at "
                                                                  "argument " +
                                         std::to_string(I));
    return Args[I]->getValueAsInt("Value");
  };
  auto Ty = [&](size_t I) {
    std::vector<std::string> Ts;
    if (I >= Args.size())
      PrintFatalError(Rule.getLoc(), "rule '" + Method + "' is missing argument " +
                                         std::to_string(I));
    collectTypes(*Args[I], Ts);
    if (Ts.size() != 1)
      PrintFatalError(Rule.getLoc(), "rule '" + Method + "' expects one type at "
                                                            "argument " +
                                         std::to_string(I));
    return Ts.front();
  };

  if (Builder == "BuilderLegalFor") {
    for (const Record *A : Args) {
      if (A->isSubClassOf("LegalizeTypePair")) {
        std::vector<TypeAlt> T0, T1;
        std::string Why0, Why1;
        if (!typeAlternatives(Records, *A->getValueAsDef("Type0"), T0, Why0) ||
            !typeAlternatives(Records, *A->getValueAsDef("Type1"), T1, Why1))
          PrintFatalError(Rule.getLoc(),
                          "legalFor pair names a type with no literal Core IR "
                          "form");
        for (const TypeAlt &X : T0)
          for (const TypeAlt &Y : T1)
            Out.push_back({"Legal", 0,
                           WithPred(guardAnd(
                               conjoin(X.G, Y.G),
                               allOf({typeIs(0, X.Type), typeIs(1, Y.Type)}))),
                           ""});
      } else {
        std::vector<TypeAlt> Ts;
        std::string Why1;
        if (!typeAlternatives(Records, *A, Ts, Why1))
          PrintFatalError(Rule.getLoc(),
                          "legalFor names a type with no literal Core IR "
                          "form");
        for (const TypeAlt &T : Ts)
          Out.push_back(
              {"Legal", 0, WithPred(guardAnd(T.G, typeIs(0, T.Type))), ""});
      }
    }
    return;
  }
  if (Builder == "BuilderLegalForCartesianProduct") {
    std::vector<ListAlt> L0, L1;
    std::string Why0, Why1;
    if (!listAlternatives(Records, *Args[0], L0, Why0) ||
        !listAlternatives(Records, *Args[1], L1, Why1))
      PrintFatalError(Rule.getLoc(),
                      "legalForCartesianProduct names a list with no literal "
                      "Core IR form");
    // A type both arms of a choice name is legal under either condition, so
    // it is emitted once with no guard; only the arm-specific types keep one.
    for (const auto &A : mergeTypeGuards(L0))
      for (const auto &B : mergeTypeGuards(L1))
        Out.push_back({"Legal", 0,
                       WithPred(joinPredicate(
                           joinPredicate(A.first, B.first),
                           allOf({typeIs(0, A.second), typeIs(1, B.second)}))),
                       ""});
    return;
  }
  if (Builder == "BuilderLegalForTypesWithMemDesc") {
    for (const Record *A : Args) {
      std::string T0 = normalizeLlt(
          A->getValueAsDef("Type0")->getValueAsString("Expr"));
      std::string T1 = normalizeLlt(
          A->getValueAsDef("Type1")->getValueAsString("Expr"));
      std::string Mem = normalizeLlt(
          A->getValueAsDef("MemTy")->getValueAsString("Expr"));
      unsigned MemBits = lltBits(Mem);
      if (MemBits == 0)
        PrintFatalError(A->getLoc(),
                        "memdesc memory type '" + Mem + "' has no "
                        "static bit size");
      // The memory descriptor is its own query axis; the C++ builder passes
      // MMOIdx 0, not type index 2.
      unsigned Align = A->getValueAsInt("Align");
      Out.push_back({"Legal", 0,
                     WithPred(allOf({typeIs(0, T0), typeIs(1, T1),
                                     memSize(0, MemBits, Align)})),
                     ""});
    }
    return;
  }
  if (Builder == "BuilderAlwaysLegal") {
    Out.push_back({"Legal", 0, WithPred(""), ""});
    return;
  }
  // "Unsupported" is not the same observation as "no rule matched": the
  // evaluator reports the first as a rule with action Unsupported and the
  // second as no_match, and diff_core.py counts them apart.  Omitting the
  // builder would have made the explicit form unexpressible.
  if (Builder == "BuilderUnsupported") {
    Out.push_back({"Unsupported", 0, WithPred(""), ""});
    return;
  }
  if (Builder == "BuilderUnsupportedFor") {
    for (const Record *A : Args) {
      std::vector<std::string> Ts;
      collectTypes(*A, Ts);
      for (const std::string &T : Ts)
        Out.push_back({"Unsupported", 0, WithPred(typeIs(0, T)), ""});
    }
    return;
  }
  // customIf(guard): the guard IS the rule, so it becomes the Core IR
  // predicate and the action is the builder's.  Before the predicate AST
  // could hold type leaves this had no literal form, which is why the X86
  // rules that read the query were all escapes.
  if (Builder == "BuilderCustomIf" && Args.empty()) {
    Out.push_back({"Custom", 0, WithPred(""), ""});
    return;
  }
  // legalIf(guard) is the "already legal under this guard" form.  It was
  // missing here while customIf and lowerIf were present, so a rule written
  // with it had no Core IR form and the whole rule set escaped -- including
  // the parts of it the vocabulary could describe.
  if (Builder == "BuilderLegalIf" && Args.empty()) {
    Out.push_back({"Legal", 0, WithPred(""), ""});
    return;
  }
  if (Builder == "BuilderLowerIf" && Args.empty()) {
    Out.push_back({"Lower", 0, WithPred(""), ""});
    return;
  }

  if (Builder == "BuilderLower") {
    Out.push_back({"Lower", 0, WithPred(""), ""});
    return;
  }
  if (Builder == "BuilderLowerFor") {
    for (const Record *A : Args) {
      std::string T0 =
          normalizeLlt(A->getValueAsDef("Type0")->getValueAsString("Expr"));
      std::string T1 =
          normalizeLlt(A->getValueAsDef("Type1")->getValueAsString("Expr"));
      Out.push_back({"Lower", 0,
                     WithPred(allOf({typeIs(0, T0), typeIs(1, T1)})), ""});
    }
    return;
  }
  if (Builder == "BuilderLibcall") {
    Out.push_back({"Libcall", 0, WithPred(""), ""});
    return;
  }
  if (Builder == "BuilderLibcallFor" || Builder == "BuilderCustomFor") {
    StringRef Action = Builder == "BuilderLibcallFor" ? "Libcall" : "Custom";
    for (const Record *A : Args) {
      std::vector<std::string> Ts;
      collectTypes(*A, Ts);
      for (const std::string &T : Ts)
        Out.push_back({Action.str(), 0, WithPred(typeIs(0, T)), ""});
    }
    return;
  }
  if (Builder == "BuilderCustomForCartesianProduct") {
    std::vector<std::string> T0, T1;
    collectTypes(*Args[0], T0);
    collectTypes(*Args[1], T1);
    for (const std::string &A : T0)
      for (const std::string &B : T1)
        Out.push_back({"Custom", 0,
                       WithPred(allOf({typeIs(0, A), typeIs(1, B)})), ""});
    return;
  }
  if (Builder == "BuilderClampScalar" || Builder == "BuilderClampScalarOrElt") {
    unsigned Idx = Int(0);
    std::vector<TypeAlt> Min, Max;
    std::string Why0, Why1;
    if (!typeAlternatives(Records, *Args[1], Min, Why0) ||
        !typeAlternatives(Records, *Args[2], Max, Why1))
      PrintFatalError(Rule.getLoc(),
                      "clampScalar bound names a type with no literal Core IR "
                      "form");
    // minScalar().maxScalar(): every widening branch first, then every
    // narrowing branch, which is the order the C++ builder produces.
    for (const TypeAlt &T : Min)
      Out.push_back(
          {"WidenScalar", Idx,
           WithPred(guardAnd(T.G, scalarNarrowerThan(Idx, lltBits(T.Type)))),
           mutationJson(Idx, "set_elem_type", T.Type)});
    for (const TypeAlt &T : Max)
      Out.push_back(
          {"NarrowScalar", Idx,
           WithPred(guardAnd(T.G, scalarWiderThan(Idx, lltBits(T.Type)))),
           mutationJson(Idx, "set_elem_type", T.Type)});
    return;
  }
  if (Builder == "BuilderMinScalar") {
    unsigned Idx = Int(0);
    std::vector<TypeAlt> Bounds;
    std::string Why1;
    if (!typeAlternatives(Records, *Args[1], Bounds, Why1))
      PrintFatalError(Rule.getLoc(),
                      "minScalar bound names a type with no literal Core IR "
                      "form");
    for (const TypeAlt &T : Bounds)
      Out.push_back(
          {"WidenScalar", Idx,
           WithPred(guardAnd(T.G, scalarNarrowerThan(Idx, lltBits(T.Type)))),
           mutationJson(Idx, "set_elem_type", T.Type)});
    return;
  }
  if (Builder == "BuilderMaxScalar") {
    unsigned Idx = Int(0);
    std::vector<TypeAlt> Bounds;
    std::string Why1;
    if (!typeAlternatives(Records, *Args[1], Bounds, Why1))
      PrintFatalError(Rule.getLoc(),
                      "maxScalar bound names a type with no literal Core IR "
                      "form");
    for (const TypeAlt &T : Bounds)
      Out.push_back(
          {"NarrowScalar", Idx,
           WithPred(guardAnd(T.G, scalarWiderThan(Idx, lltBits(T.Type)))),
           mutationJson(Idx, "set_elem_type", T.Type)});
    return;
  }
  if (Builder == "BuilderWidenScalarToNextPow2" ||
      Builder == "BuilderWidenScalarOrEltToNextPow2") {
    unsigned Idx = Int(0);
    // The floor is part of the rule: nextPow2OrMin without its Min cannot be
    // evaluated, so the value carries it.
    std::string Target =
        Args.size() > 1 ? "nextPow2OrMin" + std::to_string(Int(1))
                        : std::string("nextPow2");
    // The guard is not "true".  The C++ builder fires only when the size is
    // not a power of two, and the scalar-only builder and the OrElt variant
    // differ on vectors.
    std::string Guard = Builder == "BuilderWidenScalarToNextPow2"
                            ? sizeNotPow2(Idx)
                            : scalarOrEltSizeNotPow2(Idx);
    Out.push_back({"WidenScalar", Idx, WithPred(Guard),
                   mutationJson(Idx, "set_elem_bits", Target)});
    return;
  }
  if (Builder == "BuilderScalarSameSizeAs") {
    unsigned Idx = Int(0);
    unsigned Other = Int(1);
    std::string Ref = "sameAsSlot" + std::to_string(Other);
    // minScalarSameAs().maxScalarSameAs(): each direction has its own guard.
    Out.push_back({"WidenScalar", Idx,
                   WithPred(scalarSizeLargerThanSlot(Other, Idx)),
                   mutationJson(Idx, "set_elem_bits", Ref)});
    Out.push_back({"NarrowScalar", Idx,
                   WithPred(scalarSizeSmallerThanSlot(Other, Idx)),
                   mutationJson(Idx, "set_elem_bits", Ref)});
    return;
  }
  if (Builder == "BuilderScalarize" || Builder == "BuilderScalarizeIf") {
    unsigned Idx = Int(0);
    Out.push_back({"FewerElements", Idx, WithPred(isVector(Idx)),
                   mutationJsonInt(Idx, "set_lanes", 1)});
    return;
  }
  if (Builder == "BuilderMoreElementsToNextPow2") {
    unsigned Idx = Int(0);
    Out.push_back({"MoreElements", Idx, WithPred(numElementsNotPow2(Idx)),
                   mutationJson(Idx, "set_lanes", "nextPow2")});
    return;
  }
  if (Builder == "BuilderClampNumElements") {
    unsigned Idx = Int(0);
    // clampNumElements(Idx, MinTy, MaxTy) is the pair
    // clampMinNumElements(Idx, MinTy, lanes(MinTy)).
    //   clampMaxNumElements(Idx, MaxTy, lanes(MaxTy)),
    // so each direction names its own lane count instead of "nextPow2".
    // A bound that is a feature conditional contributes one rule per branch,
    // each with its own element type and lane count.
    std::vector<TypeAlt> Min, Max;
    std::string Why0, Why1;
    if (!typeAlternatives(Records, *Args[1], Min, Why0) ||
        !typeAlternatives(Records, *Args[2], Max, Why1))
      PrintFatalError(Rule.getLoc(),
                      "clampNumElements bound names a type with no literal "
                      "Core IR form");
    for (const TypeAlt &T : Min)
      Out.push_back(
          {"MoreElements", Idx,
           WithPred(guardAnd(
               T.G, allOf({elementTypeIs(Idx, lltElt(T.Type)),
                           fewerElementsThan(Idx, lltLanes(T.Type))}))),
           mutationJsonInt(Idx, "set_lanes", lltLanes(T.Type))});
    for (const TypeAlt &T : Max)
      Out.push_back(
          {"FewerElements", Idx,
           WithPred(
               guardAnd(T.G, allOf({elementTypeIs(Idx, lltElt(T.Type)),
                                    moreElementsThan(Idx, lltLanes(T.Type))}))),
           mutationJsonInt(Idx, "set_lanes", lltLanes(T.Type))});
    return;
  }
  if (Builder == "BuilderClampMinNumElements" ||
      Builder == "BuilderClampMaxNumElements") {
    const bool Min = Builder == "BuilderClampMinNumElements";
    unsigned Idx = Int(0);
    std::string Elt = Ty(1);
    if (Args.size() < 3 || !Args[2]->isSubClassOf("LegalizeRawArg"))
      PrintFatalError(Rule.getLoc(),
                      std::string(Method) + " expects its lane count as the "
                                            "third argument");
    std::string BoundText = resolveLaneBound(
        Rule, Args[2]->getValueAsString("Text"));
    StringRef Bound = BoundText;
    // The C++ predicates are isFixedVector() && elementType() == EltTy &&
    // numElements() < / > N.  All three parts travel: dropping isVector would
    // let a scalar match a rule about vectors, and dropping the element type
    // would let v8s16 answer for v8s8.
    Out.push_back({Min ? "MoreElements" : "FewerElements", Idx,
                   WithPred(allOf({isVector(Idx), elementTypeIs(Idx, Elt),
                                   Min ? fewerElementsThanExpr(Idx, Bound)
                                       : moreElementsThanExpr(Idx, Bound)})),
                   mutationJsonLanes(Idx, Bound)});
    return;
  }

  PrintFatalError(Rule.getLoc(),
                  "no Core IR expansion for builder '" + Method +
                      "'; add it to LegalizerInfo.td "
                      "first");
}

} // namespace

//===----------------------------------------------------------------------===//
// Shared walk
//===----------------------------------------------------------------------===//

std::vector<const Record *> LegalizerInfoEmitter::validatedRuleSets() {
  std::vector<const Record *> RuleSets =
      Records.getAllDerivedDefinitions("LegalizeRuleSet");
  if (RuleSets.empty())
    PrintFatalError("no LegalizeRuleSet records found");
  llvm::sort(RuleSets, LessRecordByID());

  // Both emitters read the same records, so both have to apply the same
  // checks.  Leaving them to -gen-legalizer meant -gen-legalizer-core accepted
  // a rule that the other output rejected, and the two then disagreed about
  // whether the description was even well formed.
  //
  // Those checks live in two places: verifyRule() for a rule, and emitRuleSet()
  // for the rule set that holds it.  Only the first was applied here, so an
  // empty rule set was rejected by -gen-legalizer and silently accepted by
  // -gen-legalizer-core.  Repeat the rule-set half rather than calling
  // emitRuleSet(), which would write the C++ fragment to this stream.
  StringSet<> SeenOpcodes;
  for (const Record *RuleSet : RuleSets) {
    std::vector<StringRef> Opcodes =
        RuleSet->getValueAsListOfStrings("Opcodes");
    if (Opcodes.empty())
      PrintFatalError(RuleSet->getLoc(), "legalizer rule set has no opcodes");
    if (RuleSet->getValueAsListOfDefs("Rules").empty())
      PrintFatalError(RuleSet->getLoc(), "legalizer rule set has no rules");
    for (StringRef Opcode : Opcodes) {
      if (!Opcode.starts_with("G_"))
        PrintFatalError(RuleSet->getLoc(),
                        "'" + Opcode + "' is not a generic opcode");
      if (!SeenOpcodes.insert(Opcode).second)
        PrintFatalError(RuleSet->getLoc(), "opcode '" + Opcode +
                                               "' is described by more than "
                                               "one rule set");
    }
    for (const Record *Rule : RuleSet->getValueAsListOfDefs("Rules"))
      verifyRule(*Rule);
  }
  return RuleSets;
}

std::vector<std::string> LegalizerInfoEmitter::domainTypes() {
  std::vector<const Record *> Types =
      Records.getAllDerivedDefinitions("LegalizeNamedType");
  std::vector<std::string> DomainTypes;
  for (const Record *T : Types) {
    std::string Norm = normalizeLlt(T->getValueAsString("Expr"));
    if (!Norm.empty())
      DomainTypes.push_back(Norm);
  }
  // The pointer type is decided by the configuration, not by a type record, so
  // it never shows up above.  A query over a ptype slot still has to be inside
  // the declared domain.
  DomainTypes.push_back("p0:" + std::to_string(CorePointerWidth));
  llvm::sort(DomainTypes);
  DomainTypes.erase(std::unique(DomainTypes.begin(), DomainTypes.end()),
                    DomainTypes.end());
  return DomainTypes;
}

//===----------------------------------------------------------------------===//
// Table output (-gen-legalizer-table)
//
// The product of the table work: the key partition and one descriptor per rule
// set, emitted from the same records the Core IR dump is built from.  The cells
// are deliberately absent -- a cell is a subtarget-specific answer, so the
// consumer fills them from its own rule chain (X86LegalizerTable::fill).
//===----------------------------------------------------------------------===//

namespace {

const char *className(int Kind) {
  switch (Kind) {
  case 0:
    return "Kind_Pointer";
  case 1:
    return "Kind_Scalar";
  case 2:
    return "Kind_Vector";
  default:
    return "Kind_PointerVector";
  }
}

/// (kind, element bits, lanes) of a literal LLT.  This is the class the key is
/// cut at, so a spelling that does not parse is an error, not a default.
bool parseTableClass(StringRef S, int &Kind, unsigned &Elem, unsigned &Lanes) {
  if (S.starts_with("p")) {
    unsigned Bits = lltBits(S);
    if (!Bits)
      return false;
    Kind = 0;
    Elem = Bits;
    Lanes = 1;
    return true;
  }
  if (S.starts_with("v")) {
    unsigned Total = lltBits(S);
    unsigned Count = lltLanes(S);
    if (!Total || !Count)
      return false;
    Kind = S.contains('p') ? 3 : 2;
    Elem = Total / Count;
    Lanes = Count;
    return true;
  }
  if (S.starts_with("s")) {
    unsigned Bits = lltBits(S);
    if (!Bits)
      return false;
    Kind = 1;
    Elem = Bits;
    Lanes = 1;
    return true;
  }
  return false;
}

/// \p SlotTypes, when not null, collects the type spellings a typeIs/typeIn
/// leaf names, per slot.  The table self-check needs a domain that the rule
/// chain actually accepts: enumerating every class for every slot asks the
/// chain about queries it never sees (a pointer where a scalar is required),
/// which is a crash in the chain, not a disagreement about the table.
void collectPredLeaves(
    StringRef Json, std::set<unsigned> &Slots, std::set<unsigned> &MemSizes,
    std::map<unsigned, std::set<std::string>> *SlotTypes = nullptr) {
  // Leaf kinds that read a type slot, and where the slot index sits.
  static const std::set<StringRef> SlotArg0 = {"typeIs",
                                               "typeIn",
                                               "isVector",
                                               "sizeIs",
                                               "sizeNotPow2",
                                               "scalarOrEltSizeNotPow2",
                                               "numElementsNotPow2",
                                               "scalarNarrowerThan",
                                               "scalarWiderThan",
                                               "elementTypeIs",
                                               "fewerElementsThan",
                                               "moreElementsThan"};
  // Two leaves take a second slot: the slot they compare against.
  static const std::set<StringRef> SlotArg1 = {"scalarSizeLargerThanSlot",
                                               "scalarSizeSmallerThanSlot"};

  size_t Pos = 0;
  while ((Pos = Json.find("\"name\":\"", Pos)) != StringRef::npos) {
    size_t NameBegin = Pos + 8;
    size_t NameEnd = Json.find('"', NameBegin);
    if (NameEnd == StringRef::npos)
      PrintFatalError("predicate text is truncated at " +
                      std::to_string(NameBegin));
    StringRef Name = Json.substr(NameBegin, NameEnd - NameBegin);
    Pos = NameEnd;

    // The integer arguments, up to the first non-numeric one.  memSize is
    // (slot, size, align), typeIs is (slot, "s8"), typeIn is (slot, [...]) --
    // so the leading integers are exactly the part the key needs.
    size_t ArgsAt = Json.find("\"args\":[", NameEnd);
    if (ArgsAt == StringRef::npos)
      PrintFatalError("predicate leaf '" + Name + "' has no arguments");
    std::vector<unsigned> Leading;
    size_t I = ArgsAt + 8; // just past '['
    while (I < Json.size()) {
      while (I < Json.size() && (Json[I] == ' ' || Json[I] == ','))
        ++I;
      size_t Start = I;
      while (I < Json.size() && isDigit(Json[I]))
        ++I;
      if (I == Start)
        break;
      Leading.push_back(std::stoul(Json.substr(Start, I - Start).str()));
    }

    if (Name == "true" || Name == "opaque" || Name == "hasFeature")
      continue;
    if (SlotTypes && (Name == "typeIs" || Name == "typeIn")) {
      if (Leading.empty())
        PrintFatalError("predicate leaf '" + Name + "' has no slot index");
      // The spellings are the string literals inside the argument list; a
      // type spelling never contains a quote.  Scan to the matching close
      // bracket so typeIn's nested list is included.
      size_t End = ArgsAt + 8; // just past '['
      unsigned Depth = 1;
      while (End < Json.size() && Depth) {
        if (Json[End] == '[')
          ++Depth;
        else if (Json[End] == ']')
          --Depth;
        ++End;
      }
      if (Depth)
        PrintFatalError("predicate leaf '" + Name +
                        "' has unbalanced brackets");
      std::set<std::string> &Out = (*SlotTypes)[Leading[0]];
      size_t Q = ArgsAt + 8; // skip the "args":[ spelling
      while ((Q = Json.find('"', Q)) != StringRef::npos && Q < End) {
        size_t E = Json.find('"', Q + 1);
        if (E == StringRef::npos || E > End)
          PrintFatalError("predicate leaf '" + Name +
                          "' has an unterminated "
                          "type spelling");
        Out.insert(Json.substr(Q + 1, E - Q - 1).str());
        Q = E + 1;
      }
    }
    if (Name == "memSize") {
      if (Leading.size() < 3)
        PrintFatalError("memSize must carry (slot, size, align)");
      if (Leading[2] != 1)
        PrintFatalError("memSize with align " + std::to_string(Leading[2]) +
                        ": the key is built for align 1");
      MemSizes.insert(Leading[1]);
      continue;
    }
    if (SlotArg0.count(Name) || SlotArg1.count(Name)) {
      if (Leading.empty())
        PrintFatalError("predicate leaf '" + Name + "' has no slot index");
      Slots.insert(Leading[0]);
      if (SlotArg1.count(Name)) {
        if (Leading.size() < 2)
          PrintFatalError("predicate leaf '" + Name + "' has no second slot");
        Slots.insert(Leading[1]);
      }
      continue;
    }
    PrintFatalError("predicate leaf '" + Name +
                    "' is not one this table knows how to key");
  }
}

} // namespace

void LegalizerInfoEmitter::runTable(raw_ostream &OS) {
  CurrentRK = &Records;
  verifyLattices();
  std::vector<const Record *> RuleSets = validatedRuleSets();
  std::vector<std::string> Types = domainTypes();

  struct TableRec {
    const Record *Def = nullptr;
    std::vector<StringRef> Opcodes;
    std::vector<int> Slots;
    std::vector<unsigned> Mem;
    // Type spellings the rules name per slot.  They seed the slot's share of
    // the self-check domain; the full class set is too wide to put in a slot
    // the rules only ever see scalars in.
    std::map<unsigned, std::set<std::string>> SlotTypes;
    // Global class ids the self-check enumerates per keyed slot, parallel to
    // Slots.  Empty for the mem axis, whose sizes travel in Mem.
    std::vector<std::vector<unsigned>> SlotDomains;
    unsigned FirstRule = 0, NumRules = 0;
  };
  std::vector<TableRec> Tables;
  std::vector<StringRef> EscapeRuleSets;
  unsigned RuleCursor = 0;
  StringSet<> SeenOpcodes;
  for (const Record *RuleSet : RuleSets) {
    std::set<unsigned> SlotSet, MemSet;
    std::map<unsigned, std::set<std::string>> SlotTypes;
    unsigned Count = 0;
    bool HasEscape = false;
    for (const Record *Rule : RuleSet->getValueAsListOfDefs("Rules")) {
      std::vector<CoreRule> Expanded;
      std::string Why;
      expandRule(Records, *Rule, Expanded, Why);
      if (!Why.empty()) {
        // Every escape is opaque as far as the key is concerned: the rule
        // produced no core rule, so whatever slots and types it reads are
        // missing from the key.  An opaque rule is the obvious case; a
        // non-literal argument list is the same problem one level down.
        // Measured: X86_PTRTOINT's legalForCartesianProduct escapes as
        // type_list_choice, which left slot 1 out of its key, and the table
        // then answered a slot-0-only question the chain answers with slot 1.
        HasEscape = true;
        continue;
      }
      for (const CoreRule &CR : Expanded) {
        collectPredLeaves(CR.Predicate, SlotSet, MemSet, &SlotTypes);
        ++Count;
      }
    }
    // A rule set with an escape has a predicate whose key dependencies this
    // emitter cannot see.  Filling from the chain would still produce an
    // answer, but the cell would stand for every query that shares the core
    // rules' key -- including queries the escaped rule distinguishes.
    // Do not tabulate it; the chain answers every query for the rule set.
    if (HasEscape) {
      EscapeRuleSets.push_back(RuleSet->getName());
      continue;
    }
    // Same rule as the dump: a rule set whose every rule escaped has no table,
    // because it has nothing to answer with.
    if (!Count)
      continue;

    TableRec T;
    T.Def = RuleSet;
    T.Opcodes = RuleSet->getValueAsListOfStrings("Opcodes");
    T.SlotTypes = std::move(SlotTypes);
    T.Slots.assign(SlotSet.begin(), SlotSet.end());
    if (T.Slots.size() > 2) {
      T.Slots.resize(2);
      // More than two slots would silently drop a dimension of the key.
      PrintFatalError(RuleSet->getLoc(),
                      "rule set " + RuleSet->getName() +
                          " reads more than two type slots; the key has room "
                          "for two and the mem axis");
    }
    // A rule set whose rules read no slot -- AlwaysLegal, Lower, Libcall --
    // has no type-dependent answer, so its table has no keyed axis at all: one
    // cell, Dims {1,1,1}.  Defaulting the slot to 0 instead would key such a
    // table on a type the query may not even have (G_BR carries none), and
    // every one of those queries would land Outside: measured, 60 of 174
    // refusals over the MIR corpus were exactly that.
    T.Mem.assign(MemSet.begin(), MemSet.end());
    if (T.Slots.empty() && !T.Mem.empty())
      PrintFatalError(RuleSet->getLoc(),
                      "rule set " + RuleSet->getName() +
                          " keys only the mem axis; the consumer needs a type "
                          "slot in front of it");
    if (!T.Mem.empty())
      T.Slots.push_back(-1);
    T.FirstRule = RuleCursor;
    T.NumRules = Count;
    RuleCursor += Count;
    for (StringRef Opcode : T.Opcodes)
      if (!SeenOpcodes.insert(Opcode).second)
        PrintFatalError(RuleSet->getLoc(), "opcode '" + Opcode +
                                               "' is described by more than "
                                               "one rule set");
    Tables.push_back(std::move(T));
  }

  // A type a rule names is a class, whether or not a named type record spells
  // the same literal.  Adding them here rather than failing keeps the class
  // set a property of the rules, not of a separate list that could omit one.
  for (const TableRec &R : Tables)
    for (const auto &Entry : R.SlotTypes)
      for (const std::string &S : Entry.second)
        Types.push_back(S);
  llvm::sort(Types);
  Types.erase(std::unique(Types.begin(), Types.end()), Types.end());

  struct TableClass {
    int Kind;
    unsigned Elem, Lanes;
    std::string Spell;
  };
  std::vector<TableClass> Classes;
  for (const std::string &S : Types) {
    int Kind = 0;
    unsigned Elem = 0, Lanes = 0;
    if (!parseTableClass(S, Kind, Elem, Lanes))
      PrintFatalError("type '" + S + "' has no (kind, element, lanes) form");
    Classes.push_back({Kind, Elem, Lanes, S});
  }
  llvm::sort(Classes, [](const TableClass &A, const TableClass &B) {
    if (A.Kind != B.Kind)
      return A.Kind < B.Kind;
    if (A.Elem != B.Elem)
      return A.Elem < B.Elem;
    return A.Lanes < B.Lanes;
  });
  Classes.erase(std::unique(Classes.begin(), Classes.end(),
                            [](const TableClass &A, const TableClass &B) {
                              return A.Kind == B.Kind && A.Elem == B.Elem &&
                                     A.Lanes == B.Lanes;
                            }),
                Classes.end());

  auto ClassID = [&Classes](StringRef S) -> unsigned {
    int Kind = 0;
    unsigned Elem = 0, Lanes = 0;
    if (!parseTableClass(S, Kind, Elem, Lanes))
      PrintFatalError("type '" + S + "' has no (kind, element, lanes) form");
    for (unsigned I = 0; I != Classes.size(); ++I)
      if (Classes[I].Kind == Kind && Classes[I].Elem == Elem &&
          Classes[I].Lanes == Lanes)
        return I;
    llvm_unreachable("mentioned type has no class");
  };

  // The self-check domain is per slot, not the product of the whole class set.
  // Most slots only ever see one kind of type: a load's pointer operand is
  // never a vector, and a scalar arithmetic source is never a pointer.  Asking
  // the chain about a combination the IR cannot produce is not a check -- it
  // asserts inside the chain (LLT::changeElementSize on a pointer), or it
  // measures a question no rule set is written to answer.
  for (TableRec &R : Tables) {
    R.SlotDomains.resize(R.Slots.size());
    for (size_t K = 0; K != R.Slots.size(); ++K) {
      const int Slot = R.Slots[K];
      if (Slot < 0)
        continue; // the mem axis enumerates R.Mem directly
      std::set<unsigned> IDs;
      bool HasPointer = false, HasScalar = false;
      auto Named = R.SlotTypes.find(static_cast<unsigned>(Slot));
      if (Named != R.SlotTypes.end())
        for (const std::string &S : Named->second) {
          const unsigned ID = ClassID(S);
          IDs.insert(ID);
          if (Classes[ID].Kind == 0 || Classes[ID].Kind == 3)
            HasPointer = true;
          else if (Classes[ID].Kind == 1)
            HasScalar = true;
        }
      if (HasPointer) {
        // Pointer slots stay pointers.  A scalar where the rules spell a
        // pointer is a query the chain does not accept, not a table answer to
        // compare against.
        for (auto I = IDs.begin(); I != IDs.end();)
          if (Classes[*I].Kind == 0 || Classes[*I].Kind == 3)
            ++I;
          else
            I = IDs.erase(I);
      } else if (IDs.empty() || HasScalar) {
        // Width boundaries matter even when no rule spells them: a rule that
        // only says scalarNarrowerThan still reads every width below the bound.
        for (unsigned I = 0; I != Classes.size(); ++I)
          if (Classes[I].Kind == 1)
            IDs.insert(I);
      }
      if (IDs.empty())
        IDs.insert(0); // no named type and no scalar class
      R.SlotDomains[K].assign(IDs.begin(), IDs.end());
    }
  }

  // The names in the fragment are prefixed with the target.  A description
  // that includes Target.td says so itself; one that does not (the X86
  // prototype only includes LegalizerInfo.td) spells it in the rule set names,
  // which are <Target>_<Opcode> by convention.  Both are read, neither guessed.
  StringRef Prefix;
  if (Records.getClass("Target")) {
    std::vector<const Record *> Targets =
        Records.getAllDerivedDefinitions("Target");
    if (Targets.size() == 1)
      Prefix = Targets.front()->getName();
  }
  if (Prefix.empty())
    Prefix =
        RuleSets.front()->getName().take_until([](char C) { return C == '_'; });
  if (Prefix.empty())
    PrintFatalError("cannot tell which target this description belongs to: "
                    "no Target record and no <Target>_<Opcode> rule set name");
  std::string LowerPrefix = Prefix.lower();

  OS << "//===-- " << Prefix
     << "LegalizerTable.inc - key partitioning ---*- C++ -*-===//\n";
  OS << "// Generated by -gen-legalizer-table.  Do not edit.\n";
  OS << "// Source of truth: the legalizer rule set description.\n";
  OS << "//\n";
  OS << "// A type class is (kind, element bits, lanes).  Every predicate leaf "
        "in\n";
  OS << "// a rule set is constant on one class (07 section 2.2), which is "
        "what makes\n";
  OS << "// the key a finite index instead of a comparison chain.\n";
  if (!EscapeRuleSets.empty()) {
    OS << "//\n";
    OS << "// Not tabulated because they contain a rule the Core IR cannot "
          "express;\n";
    OS << "// the chain answers:\n";
    for (StringRef Name : EscapeRuleSets)
      OS << "//   " << Name << "\n";
  }
  OS << "\n";

  OS << "static constexpr LegalizerTypeClass " << Prefix
     << "TypeClasses[] = {\n";
  for (size_t I = 0; I != Classes.size(); ++I) {
    const TableClass &C = Classes[I];
    OS << "  /*" << format_decimal(I, 2) << "*/ {" << className(C.Kind) << ","
       << format_decimal(C.Elem, 4) << "," << format_decimal(C.Lanes, 4)
       << "},   // " << C.Spell << "\n";
  }
  OS << "};\n";
  OS << "static constexpr unsigned " << Prefix << "NumTypeClasses = sizeof("
     << Prefix << "TypeClasses) / sizeof(" << Prefix << "TypeClasses[0]);\n";
  // The classifier is generated from the same list.  A hand-written
  // equivalent would be a second copy of the class set, and the two could
  // drift apart without anything failing to build; here a duplicate -- or a
  // missing -- class is a compile error in the generated switch.
  OS << "constexpr uint64_t " << Prefix
     << "ClassKey(unsigned Kind, unsigned ElemBits, unsigned Lanes) {\n";
  OS << "  return (static_cast<uint64_t>(Kind) << 32) |\n"
     << "         (static_cast<uint64_t>(ElemBits) << 16) | Lanes;\n";
  OS << "}\n";
  OS << "static inline int " << Prefix
     << "ClassifyType(unsigned Kind, unsigned ElemBits, unsigned Lanes) {\n";
  OS << "  switch (" << Prefix << "ClassKey(Kind, ElemBits, Lanes)) {\n";
  for (size_t I = 0; I != Classes.size(); ++I) {
    const TableClass &C = Classes[I];
    OS << "  case " << Prefix << "ClassKey(" << className(C.Kind) << ", "
       << C.Elem << ", " << C.Lanes << "):\n    return " << I << ";\n";
  }
  OS << "  default:\n    return -1;\n  }\n}\n\n";
  // A table with no keyed axis still needs three Local pointers, and Dims is 1
  // for all of them, so only element 0 is ever read.
  OS << "static constexpr uint8_t " << Prefix << "LocalTrivial[] = {0};\n\n";

  for (size_t T = 0; T != Tables.size(); ++T) {
    const TableRec &R = Tables[T];
    for (size_t K = 0; K != R.Slots.size(); ++K) {
      // A local map is the dense index of every value the axis can take.  The
      // mem axis maps to positions in the size list, not to sizes: the sizes
      // travel in their own array (MemT) so the consumer can match them
      // exactly, while the cell index stays dense.
      const size_t Count = R.Slots[K] < 0 ? R.Mem.size() : Classes.size();
      OS << "static constexpr uint8_t " << Prefix << "LocalT" << T << "S" << K
         << "[] = {";
      for (size_t C = 0; C != Count; ++C)
        OS << (C ? ", " : "") << C;
      OS << "};\n";
      if (R.Slots[K] < 0)
        continue;
      OS << "static constexpr uint8_t " << Prefix << "DomainT" << T << "S" << K
         << "[] = {";
      const std::vector<unsigned> &Domain = R.SlotDomains[K];
      for (size_t C = 0; C != Domain.size(); ++C)
        OS << (C ? ", " : "") << Domain[C];
      OS << "};\n";
    }
    if (!R.Mem.empty()) {
      OS << "static constexpr int32_t " << Prefix << "MemT" << T << "[] = {";
      for (size_t M = 0; M != R.Mem.size(); ++M)
        OS << (M ? ", " : "") << R.Mem[M];
      OS << "};\n";
    }
  }
  OS << "\n";

  // The mem axis used to be searched by the consumer: walk MemT until the
  // access size matched, or the axis ran out.  It is the same generated list
  // the cell index comes from, so the search is emitted here as a switch over
  // the sizes the rules actually name; -1 is exactly what the exhausted scan
  // meant, and no size outside the list can reach a cell.
  OS << "static inline int " << LowerPrefix
     << "MemIndex(unsigned Table, uint64_t Bits) {\n";
  OS << "  switch (Table) {\n";
  for (size_t T = 0; T != Tables.size(); ++T) {
    const TableRec &R = Tables[T];
    if (R.Mem.empty())
      continue;
    OS << "  case " << T << ":   // " << R.Def->getName() << "\n";
    OS << "    switch (Bits) {\n";
    for (size_t M = 0; M != R.Mem.size(); ++M)
      OS << "    case " << R.Mem[M] << ":\n      return " << M << ";\n";
    OS << "    default:\n      return -1;\n    }\n";
  }
  OS << "  default:\n    return -1;\n  }\n}\n\n";
  // The self-check domain: what each keyed slot is allowed to be asked about.
  // A type slot carries the global classes it enumerates; the mem axis carries
  // a null pointer and takes its local count from Mem (Dims already holds the
  // same number).  The two arrays are emitted from the same description as the
  // tables, so they cannot go stale against a rule change.
  OS << "struct LegalizerTableDomain {\n";
  OS << "  uint8_t Counts[3];\n";
  OS << "  const uint8_t *Classes[3];   // type-slot classes; null for "
        "mem/unused\n";
  OS << "};\n";
  OS << "static constexpr LegalizerTableDomain " << Prefix
     << "TableDomains[] = {\n";
  for (size_t T = 0; T != Tables.size(); ++T) {
    const TableRec &R = Tables[T];
    OS << "  /*" << format_decimal(T, 2) << "*/ {{";
    for (size_t K = 0; K != 3; ++K) {
      unsigned Dim = 1;
      if (K < R.Slots.size())
        Dim = R.Slots[K] < 0 ? R.Mem.size() : R.SlotDomains[K].size();
      if (K)
        OS << ", ";
      OS << format_decimal(Dim, 3);
    }
    OS << "}, {";
    for (size_t K = 0; K != 3; ++K) {
      if (K)
        OS << ", ";
      if (K < R.Slots.size() && R.Slots[K] >= 0)
        OS << Prefix << "DomainT" << T << "S" << K;
      else
        OS << "nullptr";
    }
    OS << "}},   // " << R.Def->getName() << "\n";
  }
  OS << "};\n";
  OS << "static constexpr unsigned " << Prefix << "NumTableDomains = sizeof("
     << Prefix << "TableDomains) / sizeof(" << Prefix << "TableDomains[0]);\n";

  OS << "struct LegalizerTableDesc {\n";
  OS << "  uint8_t NumSlots;   // 0..3: key dimensions actually used (0 = one "
        "cell)\n";
  OS << "  int8_t Slots[3];    // type slots the key reads; -1 is the mem "
        "axis\n";
  OS << "  uint8_t Dims[3];    // local class count per slot (1 = unused)\n";
  OS << "  const uint8_t *Local[3];\n";
  OS << "  const int32_t *Mem; // access sizes (bits) of the mem axis, or "
        "null\n";
  OS << "  uint16_t FirstRule, NumRules;   // range into the rule table\n";
  OS << "};\n\n";

  OS << "static constexpr LegalizerTableDesc " << Prefix << "Tables[] = {\n";
  for (size_t T = 0; T != Tables.size(); ++T) {
    const TableRec &R = Tables[T];
    OS << "  /*" << format_decimal(T, 2) << "*/ {" << R.Slots.size() << ", {";
    for (size_t K = 0; K != 3; ++K) {
      if (K)
        OS << ", ";
      OS << (K < R.Slots.size() ? R.Slots[K] : 0);
    }
    OS << "}, {";
    for (size_t K = 0; K != 3; ++K) {
      unsigned Dim = 1;
      if (K < R.Slots.size())
        Dim = R.Slots[K] < 0 ? R.Mem.size() : Classes.size();
      if (K)
        OS << ", ";
      OS << format_decimal(Dim, 3);
    }
    OS << "}, {";
    for (size_t K = 0; K != 3; ++K) {
      if (K)
        OS << ", ";
      // Unused dimensions point at the first slot's map rather than at null:
      // Dims[K] is 1 for them, so only element 0 is ever read.  A table with
      // no slots at all has no S0, so it shares the trivial map.
      if (R.Slots.empty())
        OS << Prefix << "LocalTrivial";
      else
        OS << Prefix << "LocalT" << T << "S" << (K < R.Slots.size() ? K : 0);
    }
    OS << "}, ";
    if (R.Mem.empty())
      OS << "nullptr";
    else
      OS << Prefix << "MemT" << T;
    OS << ", " << format_decimal(R.FirstRule, 4) << ","
       << format_decimal(R.NumRules, 3) << "},   // " << R.Def->getName()
       << "\n";
  }
  OS << "};\n";
  OS << "static constexpr unsigned " << Prefix << "NumTables = sizeof("
     << Prefix << "Tables) / sizeof(" << Prefix << "Tables[0]);\n\n";

  OS << "// Opcode -> table, resolved at generation time so the consumer does "
        "not\n";
  OS << "// carry a string table.\n";
  OS << "static inline int " << LowerPrefix
     << "TableForOpcode(unsigned Opcode) {\n";
  OS << "  switch (Opcode) {\n";
  std::vector<std::pair<StringRef, unsigned>> Map;
  for (size_t T = 0; T != Tables.size(); ++T)
    for (StringRef Opcode : Tables[T].Opcodes)
      Map.push_back({Opcode, T});
  llvm::sort(Map, [](const std::pair<StringRef, unsigned> &A,
                     const std::pair<StringRef, unsigned> &B) {
    return A.first < B.first;
  });
  for (const auto &Entry : Map)
    OS << "  case TargetOpcode::" << Entry.first << ": return " << Entry.second
       << ";\n";
  OS << "  default: return -1;\n";
  OS << "  }\n}\n\n";
  OS << "// Every tabulated opcode and its table, for the self-check domain.\n";
  OS << "static constexpr struct {\n";
  OS << "  unsigned Opcode;\n";
  OS << "  const char *Name;\n";
  OS << "  uint8_t Table;\n";
  OS << "} " << Prefix << "TableOpcodes[] = {\n";
  for (const auto &Entry : Map)
    OS << "  {TargetOpcode::" << Entry.first << ", \"" << Entry.first << "\", "
       << Entry.second << "},\n";
  OS << "};\n";
  OS << "static constexpr unsigned " << Prefix << "NumTableOpcodes = sizeof("
     << Prefix << "TableOpcodes) / sizeof(" << Prefix << "TableOpcodes[0]);\n";
}

void LegalizerInfoEmitter::runCore(raw_ostream &OS) {
  CurrentRK = &Records;
  verifyLattices();

  std::vector<const Record *> RuleSets = validatedRuleSets();
  std::vector<std::string> DomainTypes = domainTypes();

  // The mem axis is declared for the same reason the type slots are: a query
  // that names an access size is inside D* only if the domain lists that size.
  // The sizes come from the rules that read one -- memory rules compare the
  // MMO size exactly (TypePairAndMemDesc::isCompatible), so "which sizes are
  // in the domain" is precisely "which sizes a rule names".
  std::set<std::pair<unsigned, unsigned>> DomainMems; // (size, min align)
  auto AddMemDesc = [&](const Record &D) {
    StringRef Expr = D.getValueAsDef("MemTy")->getValueAsString("Expr");
    // Memory types go through the same normalisation the type slots use: a
    // MemTy may be a named pointer type (X86_p0) or a vector of one
    // (LLT::fixed_vector(2, X86_p0)), and neither is a plain scalar spelling.
    std::string Norm = normalizeLlt(Expr);
    unsigned Bits = Norm.empty() ? 0 : lltBits(Norm);
    if (Bits == 0)
      PrintFatalError(D.getLoc(),
                      "memory type '" + Expr + "' has no fixed width, so it "
                      "cannot be a point on the access-size axis");
    DomainMems.insert({Bits, (unsigned)D.getValueAsInt("Align")});
  };
  for (const Record *Rule : Records.getAllDerivedDefinitions("LegalizeRule")) {
    // One builder serves both the guarded and the unguarded spelling: the
    // guard is the rule's Pred, not a different method.
    if (builderRecordName(*Rule) != "BuilderLegalForTypesWithMemDesc")
      continue;
    for (const Record *A : Rule->getValueAsListOfDefs("Args")) {
      if (A->isSubClassOf("LegalizeMemDesc"))
        AddMemDesc(*A);
      else if (A->isSubClassOf("LegalizeInlineTypeList"))
        for (const Record *E : A->getValueAsListOfDefs("Elems"))
          if (E->isSubClassOf("LegalizeMemDesc"))
            AddMemDesc(*E);
    }
  }

  std::vector<std::string> Escapes;
  auto AddEscape = [&](StringRef Layer, StringRef Kind, StringRef Reason,
                       const Record &Rule, StringRef RuleSetName,
                       unsigned RuleIndex) {
    std::string S = "    {\"layer\": ";
    S += jsonEscaped(Layer);
    S += ", \"kind\": ";
    S += jsonEscaped(Kind);
    S += ", \"reason\": ";
    S += jsonEscaped(Reason);
    S += ", \"owner\": \"X86\", \"removal\": \"express it with the Core IR "
         "vocabulary\", ";
    S += "\"ruleset\": ";
    S += jsonEscaped(RuleSetName);
    S += ", \"rule_index\": " + std::to_string(RuleIndex) + ", ";
    // The rule record is anonymous, and TableGen reports the source location
    // of a structurally identical anonymous record from whichever rule set
    // was parsed first.  The line is a hint, not an identity; attribution is
    // (ruleset, rule_index).
    S += "\"origin\": {\"source\": ";
    S += jsonEscaped(originFile(Rule));
    S += ", \"line\": ";
    S += std::to_string(originLine(Rule));
    S += "}}";
    Escapes.push_back(S);
  };
  OS << "{\n";
  OS << "  \"schema\": \"legalizer-spec-core\",\n";
  OS << "  \"version\": \"0.1\",\n";
  OS << "  \"config\": {\"triple\": ";
  jsonString(OS, CoreTriple);
  OS << ", \"cpu\": ";
  jsonString(OS, CoreCPU);
  OS << ", \"features\": [], \"pointer_width\": " << CorePointerWidth << "},\n";

  OS << "  \"domain\": {\"types\": [";
  for (size_t I = 0; I != DomainTypes.size(); ++I) {
    if (I)
      OS << ", ";
    jsonString(OS, DomainTypes[I]);
  }
  OS << "], \"imms\": [], \"mems\": [";
  bool FirstMem = true;
  for (const auto &M : DomainMems) {
    if (!FirstMem)
      OS << ", ";
    FirstMem = false;
    OS << "{\"size_bits\": " << M.first << ", \"align_bits\": " << M.second
       << "}";
  }
  OS << "], \"orderings\": [], \"q0\": []},\n";

  OS << "  \"rulesets\": [\n";
  bool FirstSet = true;
  for (const Record *RuleSet : RuleSets) {
    std::vector<StringRef> Opcodes =
        RuleSet->getValueAsListOfStrings("Opcodes");

    std::vector<std::string> RulesJson;
    StringRef RuleSetName = RuleSet->getName();
    std::vector<const Record *> Rules =
        RuleSet->getValueAsListOfDefs("Rules");
    for (size_t RuleIdx = 0; RuleIdx != Rules.size(); ++RuleIdx) {
      const Record *Rule = Rules[RuleIdx];
      std::vector<CoreRule> Expanded;
      std::string Why;
      expandRule(Records, *Rule, Expanded, Why);
      if (!Why.empty()) {
        if (Why == "opaque_rule")
          AddEscape("judgment", "opaque_rule",
                    "rule body is raw C++ (" +
                        builderName(*Rule).str() + ")",
                    *Rule, RuleSetName, RuleIdx);
        else
          AddEscape("shape_parameter", Why,
                    "argument shape has no literal Core IR form (" +
                        builderName(*Rule).str() + ")",
                    *Rule, RuleSetName, RuleIdx);
        continue;
      }
      unsigned Line = originLine(*Rule);
      for (const CoreRule &CR : Expanded) {
        std::string S = "        {\"action\":\"";
        S += CR.Action + "\", \"type_idx\":" + std::to_string(CR.TypeIdx);
        S += ", \"predicate\":" + CR.Predicate;
        if (!CR.Mutation.empty())
          S += ", \"mutation\":" + CR.Mutation;
        S += ", \"ruleset\": ";
        S += jsonEscaped(RuleSetName);
        S += ", \"rule_index\": " + std::to_string(RuleIdx);
        S += ", \"origin\": {\"source\": ";
        S += jsonEscaped(originFile(*Rule));
        S += ", \"line\": " + std::to_string(Line) + "}}";
        RulesJson.push_back(S);
      }
    }

    // A rule set whose every rule escaped has no executable content.  Emitting
    // an empty shell would violate the "no empty rule set" rule (schema
    // minItems) and say nothing that escapes[] does not already say.
    if (RulesJson.empty())
      continue;

    if (!FirstSet)
      OS << ",\n";
    FirstSet = false;
    OS << "    {\"def\": ";
    jsonString(OS, RuleSet->getName());
    OS << ", \"origin\": {\"source\": ";
    jsonString(OS, originFile(*RuleSet));
    OS << ", \"line\": " << originLine(*RuleSet) << "},\n";
    OS << "     \"opcodes\": [";
    for (size_t I = 0; I != Opcodes.size(); ++I) {
      if (I)
        OS << ", ";
      jsonString(OS, Opcodes[I]);
    }
    OS << "],\n     \"rules\": [\n";
    for (size_t I = 0; I != RulesJson.size(); ++I)
      OS << RulesJson[I] << (I + 1 == RulesJson.size() ? "\n" : ",\n");
    OS << "     ]}";
  }
  OS << "\n  ],\n";

  OS << "  \"escapes\": [";
  for (size_t I = 0; I != Escapes.size(); ++I) {
    if (I)
      OS << ",\n";
    OS << Escapes[I];
  }
  if (!Escapes.empty())
    OS << "\n  ";
  OS << "]\n}\n";
}
static TableGen::Emitter::OptClass<LegalizerInfoEmitter>
    X("gen-legalizer", "Generate GlobalISel legalizer rule fragments");

/// OptClass<T> always calls T::run(), so the Core IR output needs its own
/// front-end class that forwards to LegalizerInfoEmitter::runCore().
class LegalizerCoreEmitter {
  const RecordKeeper &Records;

public:
  explicit LegalizerCoreEmitter(const RecordKeeper &RK) : Records(RK) {}
  void run(raw_ostream &OS) { LegalizerInfoEmitter(Records).runCore(OS); }
};

static TableGen::Emitter::OptClass<LegalizerCoreEmitter>
    XCore("gen-legalizer-core",
          "Emit GlobalISel legalizer rules as Core IR JSON");

/// Same reason as the Core IR output: OptClass<T> calls T::run().
class LegalizerTableEmitter {
  const RecordKeeper &Records;

public:
  explicit LegalizerTableEmitter(const RecordKeeper &RK) : Records(RK) {}
  void run(raw_ostream &OS) { LegalizerInfoEmitter(Records).runTable(OS); }
};

static TableGen::Emitter::OptClass<LegalizerTableEmitter>
    XTable("gen-legalizer-table",
           "Emit the legalizer key partition and table descriptors");
