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
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/CommandLine.h"
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
#include <tuple>

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
  void emitArg(const Record &Arg, raw_ostream &OS);
  void emitArgList(raw_ostream &OS, const Record &Rule);
  void emitRuleSet(raw_ostream &OS, const Record &RuleSet);

public:
  explicit LegalizerInfoEmitter(const RecordKeeper &RK) : Records(RK) {}
  void run(raw_ostream &OS);
  void runCore(raw_ostream &OS);
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
    return std::stoul(T.substr(Colon + 1).str());
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

std::string normalizeLlt(StringRef S) {
  StringRef T = S.trim();
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
        if (!L.empty() && !E.empty() && llvm::all_of(L, isDigit) &&
            llvm::all_of(E, isDigit))
          return "v" + L.str() + "s" + E.str();
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

/// Types[Other].getScalarSizeInBits() > Types[Idx].getScalarSizeInBits()
std::string scalarSizeLargerThanSlot(unsigned Other, unsigned Idx) {
  return "{\"kind\":\"leaf\",\"name\":\"scalarSizeLargerThanSlot\","
         "\"args\":[" + std::to_string(Other) + "," + std::to_string(Idx) +
         "]}";
}

/// Types[Other].getScalarSizeInBits() < Types[Idx].getScalarSizeInBits()
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
  // A type record whose Expr is a C++ expression has no literal Core IR form.
  // Those rules are escapes, not guesses.
  for (const Record *A : Args) {
    if (A->isSubClassOf("LegalizeTypePair")) {
      if (normalizeLlt(A->getValueAsDef("Type0")->getValueAsString("Expr")).empty() ||
          normalizeLlt(A->getValueAsDef("Type1")->getValueAsString("Expr")).empty()) {
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
      if (normalizeLlt(A->getValueAsString("Expr")).empty()) {
        Why = "nonliteral_type";
        return;
      }
      continue;
    }
    if (A->isSubClassOf("LegalizeNamedTypeList") ||
        A->isSubClassOf("LegalizeInlineTypeList")) {
      for (const Record *E : A->getValueAsListOfDefs("Elems"))
        if (normalizeLlt(E->getValueAsString("Expr")).empty()) {
          Why = "nonliteral_type";
          return;
        }
      continue;
    }
    if (A->isSubClassOf("LegalizeArgInt"))
      continue;
    if (A->isSubClassOf("LegalizeTypeListChoice")) {
      Why = "type_list_choice";
      return;
    }
    if (A->isSubClassOf("LegalizeInlineType")) {
      Why = "inline_type_expr";
      return;
    }
    if (A->isSubClassOf("LegalizeRawArg")) {
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
        std::vector<std::string> T0, T1;
        collectTypes(*A->getValueAsDef("Type0"), T0);
        collectTypes(*A->getValueAsDef("Type1"), T1);
        Out.push_back({"Legal", 0,
                       WithPred(allOf({typeIs(0, T0.front()),
                                       typeIs(1, T1.front())})),
                       ""});
      } else {
        std::vector<std::string> Ts;
        collectTypes(*A, Ts);
        for (const std::string &T : Ts)
          Out.push_back({"Legal", 0, WithPred(typeIs(0, T)), ""});
      }
    }
    return;
  }
  if (Builder == "BuilderLegalForCartesianProduct") {
    std::vector<std::string> T0, T1;
    collectTypes(*Args[0], T0);
    collectTypes(*Args[1], T1);
    for (const std::string &A : T0)
      for (const std::string &B : T1)
        Out.push_back({"Legal", 0, WithPred(allOf({typeIs(0, A), typeIs(1, B)})),
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
    std::string MinTy = Ty(1);
    std::string MaxTy = Ty(2);
    Out.push_back({"WidenScalar", Idx,
                   WithPred(scalarNarrowerThan(Idx, lltBits(MinTy))),
                   mutationJson(Idx, "set_elem_type", MinTy)});
    Out.push_back({"NarrowScalar", Idx,
                   WithPred(scalarWiderThan(Idx, lltBits(MaxTy))),
                   mutationJson(Idx, "set_elem_type", MaxTy)});
    return;
  }
  if (Builder == "BuilderMinScalar") {
    unsigned Idx = Int(0);
    std::string Ty0 = Ty(1);
    Out.push_back({"WidenScalar", Idx,
                   WithPred(scalarNarrowerThan(Idx, lltBits(Ty0))),
                   mutationJson(Idx, "set_elem_type", Ty0)});
    return;
  }
  if (Builder == "BuilderMaxScalar") {
    unsigned Idx = Int(0);
    std::string Ty0 = Ty(1);
    Out.push_back({"NarrowScalar", Idx,
                   WithPred(scalarWiderThan(Idx, lltBits(Ty0))),
                   mutationJson(Idx, "set_elem_type", Ty0)});
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
    std::string MinTy = Ty(1);
    std::string MaxTy = Ty(2);
    std::string Elt = lltElt(MinTy);
    std::string Elem = elementTypeIs(Idx, Elt);
    Out.push_back({"MoreElements", Idx,
                   WithPred(allOf({Elem, fewerElementsThan(
                                             Idx, lltLanes(MinTy))})),
                   mutationJsonInt(Idx, "set_lanes", lltLanes(MinTy))});
    Out.push_back({"FewerElements", Idx,
                   WithPred(allOf({Elem, moreElementsThan(
                                             Idx, lltLanes(MaxTy))})),
                   mutationJsonInt(Idx, "set_lanes", lltLanes(MaxTy))});
    return;
  }
  // clampMinNumElements / clampMaxNumElements take their element count as a
  // C++ expression (for example "HasBWI ? 64 : 32").  It has no literal Core
  // IR form, so those rules are escapes, not guessed rules.

  PrintFatalError(Rule.getLoc(),
                  "no Core IR expansion for builder '" + Method +
                      "'; add it to LegalizerInfo.td "
                      "first");
}

} // namespace

void LegalizerInfoEmitter::runCore(raw_ostream &OS) {
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
  OS << "], \"imms\": [], \"mems\": [], \"orderings\": [], \"q0\": []},\n";

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
//===----------------------------------------------------------------------===//
// Table output (-gen-legalizer-table)
//
// The table is a key over type classes, not a second description of the rules.
// A class is (kind, element bits, lanes); the classes come from the named types
// the rules are written in terms of, so the lattice cannot drift from the .td.
// A rule set is tabulated only when its guards are total over that key: a rule
// that reads the memory descriptor, the opcode, or a type slot past 1 is not
// tabulated and takes the chain instead.  That is the same boundary the
// prototype documented, now enforced by construction.
//===----------------------------------------------------------------------===//

namespace {

cl::opt<std::string> TablePrefix(
    "legalizer-table-prefix",
    cl::desc("identifier prefix for the generated legalizer table"),
    cl::init("X86"));

enum TableTypeKind { TablePointer, TableScalar, TableVector, TablePtrVector };

struct TableType {
  TableTypeKind Kind;
  unsigned ElemBits;
  unsigned Lanes;
  std::string Name;
};

/// Split "Cond ? Then : Else" at the top level, ignoring parentheses and
/// nested conditionals.  The condition itself is irrelevant: both branches
/// name types that must be in the lattice.
bool splitConditional(StringRef S, StringRef &Then, StringRef &Else) {
  int Depth = 0;
  size_t Q = StringRef::npos;
  for (size_t I = 0; I != S.size(); ++I) {
    char C = S[I];
    if (C == '(' || C == '[' || C == '{')
      ++Depth;
    else if (C == ')' || C == ']' || C == '}')
      --Depth;
    else if (C == '?' && Depth == 0) {
      Q = I;
      break;
    }
  }
  if (Q == StringRef::npos)
    return false;

  Depth = 0;
  int Nest = 0;
  for (size_t I = Q + 1; I != S.size(); ++I) {
    char C = S[I];
    if (C == '(' || C == '[' || C == '{')
      ++Depth;
    else if (C == ')' || C == ']' || C == '}')
      --Depth;
    else if (C == '?' && Depth == 0)
      ++Nest;
    else if (C == ':' && Depth == 0) {
      if (Nest == 0) {
        Then = S.slice(Q + 1, I);
        Else = S.substr(I + 1);
        return true;
      }
      --Nest;
    }
  }
  return false;
}

/// Strip balanced parentheses around a whole expression.
StringRef stripParens(StringRef S) {
  S = S.trim();
  while (S.size() >= 2 && S.front() == '(' && S.back() == ')') {
    int Depth = 0;
    bool Balanced = true;
    for (size_t I = 0; I != S.size(); ++I) {
      if (S[I] == '(')
        ++Depth;
      else if (S[I] == ')' && --Depth == 0 && I + 1 != S.size()) {
        Balanced = false;
        break;
      }
    }
    if (!Balanced)
      break;
    S = S.drop_front().drop_back().trim();
  }
  return S;
}

class LegalizerTableEmitterImpl {
  const RecordKeeper &Records;
  std::map<std::string, const Record *> NamedTypes;
  std::vector<TableType> Classes;

  bool resolve(StringRef Expr, StringRef Root, SmallVectorImpl<TableType> &Out,
               unsigned Depth) {
    Expr = stripParens(Expr);
    if (Depth > 16)
      return false;

    StringRef Then, Else;
    if (splitConditional(Expr, Then, Else))
      return resolve(Then, Root, Out, Depth) &&
             resolve(Else, Root, Out, Depth);

    auto ArgOf = [&](StringRef Prefix, StringRef &Arg) {
      if (!Expr.consume_front(Prefix) || !Expr.consume_back(")"))
        return false;
      Arg = Expr;
      return true;
    };

    StringRef Arg;
    if (ArgOf("LLT::scalar(", Arg)) {
      unsigned Bits;
      if (Arg.trim().getAsInteger(10, Bits))
        return false;
      Out.push_back({TableScalar, Bits, 1, Root.str()});
      return true;
    }
    if (ArgOf("LLT::pointer(", Arg)) {
      Out.push_back({TablePointer, 0, 1, Root.str()});
      return true;
    }
    if (ArgOf("LLT::fixed_vector(", Arg)) {
      size_t Comma = Arg.find(',');
      if (Comma == StringRef::npos)
        return false;
      unsigned Lanes;
      if (Arg.substr(0, Comma).trim().getAsInteger(10, Lanes))
        return false;
      StringRef Elem = Arg.substr(Comma + 1).trim();

      unsigned ElemBits;
      if (!Elem.getAsInteger(10, ElemBits)) {
        Out.push_back({TableVector, ElemBits, Lanes, Root.str()});
        return true;
      }

      SmallVector<TableType, 2> Elems;
      if (!resolve(Elem, Root, Elems, Depth + 1))
        return false;
      for (const TableType &E : Elems) {
        if (E.Kind == TableScalar)
          Out.push_back({TableVector, E.ElemBits, Lanes, Root.str()});
        else if (E.Kind == TablePointer)
          // A vector of pointers is a different LLT from a vector of integers
          // of the same shape; the class list cannot express it yet.
          Out.push_back({TablePtrVector, 0, Lanes, Root.str()});
        else
          return false;
      }
      return true;
    }

    // A named type (or a named list member) refers to another record.
    if (Expr.empty() || !(isAlpha(Expr.front()) || Expr.front() == '_'))
      return false;
    for (char C : Expr)
      if (!isAlnum(C) && C != '_')
        return false;
    auto It = NamedTypes.find(Expr.str());
    if (It == NamedTypes.end())
      return false;
    return resolve(It->second->getValueAsString("Expr"), Root, Out, Depth + 1);
  }

  void collectClasses() {
    std::vector<const Record *> Types =
        Records.getAllDerivedDefinitions("LegalizeNamedType");
    llvm::sort(Types, LessRecordByID());

    std::set<std::tuple<int, unsigned, unsigned>> Seen;
    for (const Record *T : Types) {
      SmallVector<TableType, 4> Resolved;
      SmallVector<TableType, 4> Out;
      StringRef Expr = T->getValueAsString("Expr");
      if (!resolve(Expr, T->getName(), Resolved, 0))
        PrintFatalError(T->getLoc(),
                        "cannot derive a table type class from '" + Expr + "'");
      for (const TableType &R : Resolved) {
        if (R.Kind == TablePtrVector)
          continue;
        if (!Seen.insert({R.Kind, R.ElemBits, R.Lanes}).second)
          continue;
        Out.push_back(R);
      }
      Classes.insert(Classes.end(), Out.begin(), Out.end());
    }

    auto Rank = [](TableTypeKind K) {
      switch (K) {
      case TablePointer:
        return 0;
      case TableScalar:
        return 1;
      case TableVector:
        return 2;
      case TablePtrVector:
        return 3;
      }
      llvm_unreachable("bad table type kind");
    };
    llvm::sort(Classes, [&](const TableType &A, const TableType &B) {
      if (Rank(A.Kind) != Rank(B.Kind))
        return Rank(A.Kind) < Rank(B.Kind);
      if (A.ElemBits != B.ElemBits)
        return A.ElemBits < B.ElemBits;
      return A.Lanes < B.Lanes;
    });
    if (Classes.empty() || Classes.front().Kind != TablePointer)
      PrintFatalError("a legalizer table needs a pointer type class");
  }

  static bool hasMemDescArg(const Record &Rule) {
    for (const Record *Arg : Rule.getValueAsListOfDefs("Args"))
      if (Arg->isSubClassOf("LegalizeMemDesc"))
        return true;
    return false;
  }

  static bool hasOpaqueEscape(const Record &Rule) {
    if (!Rule.isSubClassOf("LegalizeOpaqueRule"))
      return false;
    StringRef Body = Rule.getValueAsString("OpaqueArgs");
    return Body.contains("MMODescrs") || Body.contains("Query.Opcode") ||
           Body.contains("Opcode ==") || Body.contains("Types[2]") ||
           Body.contains("Types[3]");
  }

  static bool tabulated(const Record &RuleSet) {
    for (const Record *Rule : RuleSet.getValueAsListOfDefs("Rules"))
      if (hasMemDescArg(*Rule) || hasOpaqueEscape(*Rule))
        return false;
    return true;
  }

  /// C++ spelling of a resolved class.  The pointer width is a parameter of
  /// the generated collector, not a constant: the same domain is asked on
  /// 32- and 64-bit subtargets.
  static std::string render(const TableType &T) {
    switch (T.Kind) {
    case TablePointer:
      return "LLT::pointer(0, PointerBits)";
    case TableScalar:
      return "LLT::scalar(" + std::to_string(T.ElemBits) + ")";
    case TableVector:
      return "LLT::fixed_vector(" + std::to_string(T.Lanes) + ", " +
             std::to_string(T.ElemBits) + ")";
    case TablePtrVector:
      return "LLT::fixed_vector(" + std::to_string(T.Lanes) +
             ", LLT::pointer(0, PointerBits))";
    }
    llvm_unreachable("bad table type kind");
  }

  /// Type tuples a rule names as a complete query shape.  A builder that takes
  /// two separate type arguments (ClampScalar's bounds) names bounds, not a
  /// query, so only list/pair/memdesc arguments are used.
  void collectRuleTuples(const Record &Rule,
                         std::vector<std::vector<TableType>> &Out) {
    std::vector<const Record *> Args = Rule.getValueAsListOfDefs("Args");
    // A builder that takes a slot index treats its type arguments as bounds
    // (ClampScalar<0, s8, sMaxScalar>), not as a query shape.  Without an
    // index, a list of types is a legalFor-style group and each type is one
    // unary query.
    bool HasIndex = false;
    for (const Record *Arg : Args)
      if (Arg->isSubClassOf("LegalizeArgIndex"))
        HasIndex = true;

    auto ResolveOne = [&](const Record &R, std::vector<TableType> &Tys) {
      StringRef Expr = R.isSubClassOf("LegalizeInlineType")
                           ? R.getValueAsString("Text")
                           : R.getValueAsString("Expr");
      SmallVector<TableType, 2> Resolved;
      if (!resolve(Expr, R.getName(), Resolved, 0))
        PrintFatalError(R.getLoc(), "cannot resolve '" + Expr +
                                        "' for the table domain");
      Tys.insert(Tys.end(), Resolved.begin(), Resolved.end());
    };

    // Expand a list-shaped argument: a named/inline list, or a choice between
    // two named lists.
    std::function<void(const Record &, std::vector<TableType> &)> ExpandList =
        [&](const Record &List, std::vector<TableType> &L) {
          if (List.isSubClassOf("LegalizeTypeListChoice")) {
            StringRef Text = List.getValueAsString("Text");
            StringRef Then, Else;
            SmallVector<StringRef, 2> Branches;
            if (splitConditional(Text, Then, Else)) {
              Branches.push_back(Then.trim());
              Branches.push_back(Else.trim());
            } else {
              Branches.push_back(Text.trim());
            }
            for (StringRef B : Branches) {
              const Record *R = Records.getDef(B);
              if (!R || !R->isSubClassOf("LegalizeNamedTypeList"))
                PrintFatalError(List.getLoc(),
                                "cannot resolve type list '" + B + "'");
              ExpandList(*R, L);
            }
            return;
          }
          for (const Record *E : List.getValueAsListOfDefs("Elems")) {
            std::vector<TableType> Tys;
            ResolveOne(*E, Tys);
            L.insert(L.end(), Tys.begin(), Tys.end());
          }
        };

    // legalForCartesianProduct({a, b}, {c}) is a pair query, not two unary
    // ones: each argument list contributes one slot.
    if (builderName(Rule) == "legalForCartesianProduct") {
      std::vector<std::vector<TableType>> Lists;
      for (const Record *Arg : Args) {
        if (!Arg->isSubClassOf("LegalizeNamedTypeList") &&
            !Arg->isSubClassOf("LegalizeInlineTypeList") &&
            !Arg->isSubClassOf("LegalizeTypeListChoice"))
          continue;
        std::vector<TableType> L;
        ExpandList(*Arg, L);
        if (!L.empty())
          Lists.push_back(std::move(L));
      }
      std::vector<TableType> Cur;
      std::function<void(size_t)> Product = [&](size_t I) {
        if (I == Lists.size()) {
          Out.push_back(Cur);
          return;
        }
        for (const TableType &T : Lists[I]) {
          Cur.push_back(T);
          Product(I + 1);
          Cur.pop_back();
        }
      };
      Product(0);
      return;
    }

    for (const Record *Arg : Args) {
      if (Arg->isSubClassOf("LegalizeNamedTypeList") ||
          Arg->isSubClassOf("LegalizeInlineTypeList") ||
          Arg->isSubClassOf("LegalizeTypeListChoice")) {
        std::vector<TableType> L;
        ExpandList(*Arg, L);
        for (const TableType &T : L)
          Out.push_back({T});
      } else if (Arg->isSubClassOf("LegalizeTypePair")) {
        std::vector<TableType> Ty;
        ResolveOne(*Arg->getValueAsDef("Type0"), Ty);
        ResolveOne(*Arg->getValueAsDef("Type1"), Ty);
        Out.push_back(Ty);
      } else if (Arg->isSubClassOf("LegalizeMemDesc")) {
        // The memory type is part of MMODescr, not of Types; the query shape
        // the reader sees is (value, pointer).
        std::vector<TableType> Ty;
        ResolveOne(*Arg->getValueAsDef("Type0"), Ty);
        ResolveOne(*Arg->getValueAsDef("Type1"), Ty);
        Out.push_back(Ty);
      } else if (!HasIndex &&
                 (Arg->isSubClassOf("LegalizeNamedType") ||
                  Arg->isSubClassOf("LegalizeInlineType"))) {
        std::vector<TableType> Ty;
        ResolveOne(*Arg, Ty);
        for (const TableType &T : Ty)
          Out.push_back({T});
      }
    }
  }

public:
  explicit LegalizerTableEmitterImpl(const RecordKeeper &RK) : Records(RK) {
    for (const Record *T :
         Records.getAllDerivedDefinitions("LegalizeNamedType"))
      NamedTypes[T->getName().str()] = T;
  }

  void run(raw_ostream &OS) {
    collectClasses();

    std::vector<const Record *> RuleSets =
        Records.getAllDerivedDefinitions("LegalizeRuleSet");
    llvm::sort(RuleSets, LessRecordByID());
    std::vector<const Record *> Tables;
    for (const Record *RS : RuleSets)
      if (tabulated(*RS))
        Tables.push_back(RS);

    std::string P = TablePrefix;
    std::string Lower = P;
    if (!Lower.empty())
      Lower[0] = toLower(Lower[0]);

    OS << "//===-- " << P << "LegalizerTable.inc - generated table data "
          "-----*- C++ -*-===//\n";
    OS << "//\n// Generated by llvm-tblgen -gen-legalizer-table.  Do not edit.\n";
    OS << "//\n//===----------------------------------------------------------------------===//\n\n";

    OS << "static constexpr LegalizerTypeClass " << P
       << "TypeClasses[] = {\n";
    for (size_t I = 0; I != Classes.size(); ++I) {
      const TableType &C = Classes[I];
      StringRef Kind = C.Kind == TablePointer ? "Kind_Pointer"
                       : C.Kind == TableScalar ? "Kind_Scalar"
                                               : "Kind_Vector";
      OS << "  /*" << I << "*/ {" << Kind << ", " << C.ElemBits << ", "
         << C.Lanes << "}, // " << C.Name << "\n";
    }
    OS << "};\n";
    OS << "static constexpr unsigned " << P
       << "NumTypeClasses = sizeof(" << P << "TypeClasses) / sizeof(" << P
       << "TypeClasses[0]);\n\n";

    OS << "// The key is uniform for now: slots 0 and 1, partitioned by the full "
          "class list.\n";
    OS << "// A rule set that needs more than that is not in this table (see "
          "the emitter).\n";
    OS << "static constexpr LegalizerTableDesc " << P << "Tables[] = {\n";
    for (size_t I = 0; I != Tables.size(); ++I)
      OS << "  /*" << I << "*/ {{0, 1}, {" << Classes.size() << ", "
         << Classes.size() << "}}, // " << Tables[I]->getName() << "\n";
    OS << "};\n";
    OS << "static constexpr unsigned " << P << "NumTables = sizeof(" << P
       << "Tables) / sizeof(" << P << "Tables[0]);\n\n";

    OS << "static inline int " << Lower << "TableForOpcode(unsigned Opcode) {\n";
    OS << "  switch (Opcode) {\n";
    for (size_t I = 0; I != Tables.size(); ++I)
      for (StringRef Opcode : Tables[I]->getValueAsListOfStrings("Opcodes"))
        OS << "  case TargetOpcode::" << Opcode << ": return " << I << ";\n";
    OS << "  default: return -1;\n  }\n}\n";
  }

  /// Slots a predicate, an opaque body or a mutation index names.  The table
  /// key is only sound if the domain has the same arity as the guards, so a
  /// rule that reads slot 1 must not be probed with a one-type query.
  static void scanSlots(StringRef S, unsigned &Arity) {
    for (size_t I = 0; I + 2 < S.size(); ++I) {
      if (S[I] != '(')
        continue;
      size_t J = I + 1;
      while (J < S.size() && isSpace(S[J]))
        ++J;
      if (J >= S.size() || !isDigit(S[J]))
        continue;
      unsigned V = 0;
      while (J < S.size() && isDigit(S[J]))
        V = V * 10 + (S[J++] - '0');
      while (J < S.size() && isSpace(S[J]))
        ++J;
      if (J < S.size() && S[J] == ',')
        Arity = std::max(Arity, V + 1);
    }
    for (size_t P = S.find("Types["); P != StringRef::npos;
         P = S.find("Types[", P + 1)) {
      size_t J = P + 6;
      unsigned V = 0;
      while (J < S.size() && isDigit(S[J]))
        V = V * 10 + (S[J++] - '0');
      Arity = std::max(Arity, V + 1);
    }
  }

  static void scanPredicate(const Record *P, unsigned &Arity) {
    if (!P)
      return;
    if (P->isSubClassOf("LegalizeTypePredicate"))
      scanSlots(P->getValueAsString("Call"), Arity);
    else if (P->isSubClassOf("LegalizeQueryPredicate"))
      scanSlots(P->getValueAsString("Expr"), Arity);
    if (P->isSubClassOf("LegalizeQueryPredicateDag") ||
        P->isSubClassOf("LegalizePredicateDag")) {
      std::function<void(const Init *)> Walk = [&](const Init *I) {
        if (const auto *D = dyn_cast<DagInit>(I)) {
          for (const Init *Arg : D->getArgs())
            Walk(Arg);
          return;
        }
        if (const auto *Def = dyn_cast<DefInit>(I))
          if (Def->getDef() != P)
            scanPredicate(Def->getDef(), Arity);
      };
      Walk(P->getValueAsDag("CondDag"));
    }
  }

  unsigned arityOf(const Record &RuleSet) {
    unsigned Arity = 1;
    for (const Record *Rule : RuleSet.getValueAsListOfDefs("Rules")) {
      for (const Record *Arg : Rule->getValueAsListOfDefs("Args"))
        if (Arg->isSubClassOf("LegalizeArgIndex"))
          Arity = std::max(Arity, unsigned(Arg->getValueAsInt("Value") + 1));
      scanPredicate(Rule->getValueAsOptionalDef("Pred"), Arity);
      scanSlots(Rule->getValueAsString("OpaqueArgs"), Arity);
    }
    return std::min(Arity, 3u);
  }

  void runPoints(raw_ostream &OS) {
    std::vector<const Record *> RuleSets =
        Records.getAllDerivedDefinitions("LegalizeRuleSet");
    llvm::sort(RuleSets, LessRecordByID());

    struct DomainPoint {
      std::string Text;
      unsigned NumTypes;
    };

    // Opcode -> distinct query shapes, in first-seen order.
    std::vector<std::string> Opcodes;
    std::map<std::string, std::vector<DomainPoint>> Points;
    std::map<std::string, std::set<std::string>> Seen;

    for (const Record *RS : RuleSets) {
      // The domain is the domain of the emitted table: asking the chain a
      // question the table deliberately does not answer adds no evidence.
      if (!tabulated(*RS))
        continue;

      unsigned Arity = arityOf(*RS);
      std::vector<std::vector<TableType>> Tuples;
      for (const Record *Rule : RS->getValueAsListOfDefs("Rules"))
        collectRuleTuples(*Rule, Tuples);
      for (const std::vector<TableType> &Tuple : Tuples)
        Arity = std::max(Arity, unsigned(Tuple.size()));

      TableType Ptr{TablePointer, 0, 1, "p0"};
      for (StringRef Opcode : RS->getValueAsListOfStrings("Opcodes")) {
        std::string Op = Opcode.str();
        if (!Seen.count(Op))
          Opcodes.push_back(Op);
        auto &List = Points[Op];
        auto &SeenSet = Seen[Op];
        auto Add = [&](const std::vector<TableType> &Tuple) {
          if (Tuple.empty() || Tuple.size() > 3)
            return;
          std::string Key;
          for (const TableType &T : Tuple)
            Key += render(T) + "|";
          if (!SeenSet.insert(Key).second)
            return;
          std::string S = "{";
          for (size_t I = 0; I != 3; ++I) {
            if (I)
              S += ", ";
            S += I < Tuple.size() ? render(Tuple[I]) : "LLT()";
          }
          S += "}";
          List.push_back({S, static_cast<unsigned>(Tuple.size())});
        };

        // Every opcode gets a pointer probe with the arity of its guards: the
        // pointer class is the one class whose width is not in the .td.
        Add(std::vector<TableType>(Arity, Ptr));
        for (const std::vector<TableType> &Tuple : Tuples)
          if (Tuple.size() == Arity)
            Add(Tuple);
      }
    }

    unsigned Total = 0;
    for (StringRef Op : Opcodes)
      Total += Points[Op.str()].size();

    std::string P = TablePrefix;
    std::string Lower = P;
    if (!Lower.empty())
      Lower[0] = toLower(Lower[0]);
    OS << "//===-- " << P << "LegalizerTablePoints.inc - generated domain "
          "-----*- C++ -*-===//\n";
    OS << "//\n// Generated by llvm-tblgen -gen-legalizer-table-points.  Do not "
          "edit.\n";
    OS << "//\n//===----------------------------------------------------------------------===//\n\n";
    OS << "static constexpr unsigned " << P << "NumTablePoints = " << Total
       << ";\n\n";
    OS << "static void " << Lower << "CollectTablePoints(SmallVectorImpl<" << P
       << "TablePoint> &Out, unsigned PointerBits) {\n";
    for (StringRef Op : Opcodes)
      for (const DomainPoint &P : Points[Op.str()])
        OS << "  Out.push_back({TargetOpcode::" << Op << ", \"" << Op << "\", "
           << P.NumTypes << ", " << P.Text << "});\n";
    OS << "}\n";
  }
};

} // namespace

class LegalizerTableEmitter {
  const RecordKeeper &Records;

public:
  explicit LegalizerTableEmitter(const RecordKeeper &RK) : Records(RK) {}
  void run(raw_ostream &OS) {
    LegalizerTableEmitterImpl(Records).run(OS);
  }
};

static TableGen::Emitter::OptClass<LegalizerTableEmitter>
    XTable("gen-legalizer-table",
           "Emit the GlobalISel legalizer table data");

class LegalizerTablePointsEmitter {
  const RecordKeeper &Records;

public:
  explicit LegalizerTablePointsEmitter(const RecordKeeper &RK) : Records(RK) {}
  void run(raw_ostream &OS) {
    LegalizerTableEmitterImpl(Records).runPoints(OS);
  }
};

static TableGen::Emitter::OptClass<LegalizerTablePointsEmitter>
    XTablePoints("gen-legalizer-table-points",
                 "Emit the GlobalISel legalizer table domain");

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
