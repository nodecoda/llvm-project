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
  StringRef Builder = builderRecordName(Rule);
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
// rules live in llvm-dev/2026-10-28-core-ir-mapping.md; this code implements
// exactly that table and refuses anything it does not cover.
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

/// Translate the C++ spelling a type record carries into a literal LLT.
/// Returns an empty string when the expression depends on the subtarget.
/// The translation table is normative: llvm-dev/2026-10-28-core-ir-mapping.md.
bool isLiteralLlt(StringRef S);

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
/// True when the spelling is a literal LLT.  A type record whose Expr is a
/// C++ expression (LLT::fixed_vector(...), a feature ternary) is not a domain
/// element; the domain is generated separately from GenericOpcodes.td.
bool isLiteralLlt(StringRef S) {
  if (S.empty() || S.find('?') != StringRef::npos ||
      S.find(' ') != StringRef::npos || S.find("LLT::") != StringRef::npos)
    return false;
  StringRef T = S;
  if (T.consume_front("p")) {
    if (T.empty() || !isDigit(T.front()))
      return false;
    size_t Colon = T.find(':');
    if (Colon == StringRef::npos)
      return true;  // p0, address space only
    return Colon + 1 < T.size() && isDigit(T[Colon + 1]);
  }
  return lltBits(S) != 0;
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
  PrintFatalError(R.getLoc(),
                  "type predicate '" + Name + "' has no Core IR expansion; "
                  "add it to llvm-dev/2026-10-28-core-ir-mapping.md first");
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
                      "'; add it to llvm-dev/2026-10-28-core-ir-mapping.md "
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
  for (const Record *RuleSet : RuleSets)
    for (const Record *Rule : RuleSet->getValueAsListOfDefs("Rules"))
      verifyRule(*Rule);

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
  StringSet<> SeenOpcodes;
  for (const Record *RuleSet : RuleSets) {
    std::vector<StringRef> Opcodes = RuleSet->getValueAsListOfStrings("Opcodes");
    if (Opcodes.empty())
      PrintFatalError(RuleSet->getLoc(), "legalizer rule set has no opcodes");
    for (StringRef Opcode : Opcodes) {
      if (!SeenOpcodes.insert(Opcode).second)
        PrintFatalError(RuleSet->getLoc(),
                        "opcode '" + Opcode +
                            "' is described by more than one rule set");
    }

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
