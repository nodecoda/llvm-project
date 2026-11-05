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
#include <memory>
#include <set>
#include <string>
#include <tuple>

using namespace llvm;

namespace {
struct LoweringStats {
  unsigned Builders = 0;
  unsigned Overloads = 0;
  unsigned Entries = 0;
  unsigned Rules = 0;
  unsigned RuleSets = 0;
  unsigned Escapes = 0;
  unsigned EntriesWithoutTable = 0;
  unsigned RuleSetsWithoutTable = 0;
  unsigned CoreRules = 0;
  unsigned CorePrimitives = 0;
};

/// Defined after expandRule(); the -gen-legalizer emitter above it needs the
/// declaration.  Returns the counts the -gen-legalizer-check report prints.
static LoweringStats verifyLoweringRegistry(const RecordKeeper &Records);

/// Defined with the other core-IR machinery below.  verifyRule() calls it so
/// that -gen-legalizer rejects an ill-sorted primitive rule before it can
/// become C++.
static void verifyCoreRule(const RecordKeeper &Records, const Record &Rule);

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
  void emitCoreRule(const Record &Rule, raw_ostream &OS);
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
///
/// The expression names the prologue identifiers (`Is64Bit`, `X86_s32`) and,
/// for a query dag, takes a lambda whose parameter is called `Query`.  Both
/// emitters that render rules (-gen-legalizer and -gen-legalizer-table) emit
/// that same prologue, so this is shared rather than duplicated.
static void renderPredicate(const Record &P, raw_ostream &OS) {
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
  // A named type predicate is a query predicate too.  As a dag leaf it is
  // emitted by the walker above; standing alone it is the callable its Call
  // text names.
  if (P.isSubClassOf("LegalizeTypePredicate")) {
    OS << squashSpace(P.getValueAsString("Call"));
    return;
  }
  PrintFatalError(&P, "unknown LegalizePredicate record");
}

void LegalizerInfoEmitter::emitPredicate(const Record &P, raw_ostream &OS) {
  renderPredicate(P, OS);
}

/// `(A ? X : (B ? Y : Z))` from a priority list.  Used for a select type, a
/// select type list and a select lane count, so the three spellings cannot
/// disagree about which guard wins.
static std::string selectText(const Record &Select, StringRef ThenField,
                              StringRef ElseField) {
  std::vector<const Record *> Conds = Select.getValueAsListOfDefs("Conds");
  std::vector<const Record *> Thens = Select.getValueAsListOfDefs(ThenField);
  if (Conds.size() != Thens.size())
    PrintFatalError(Select.getLoc(), "'" + Select.getName() +
                                         "' has " + std::to_string(Conds.size()) +
                                         " guards but " +
                                         std::to_string(Thens.size()) + " values");
  std::string Expr = Select.getValueAsDef(ElseField)->getName().str();
  for (size_t I = Conds.size(); I-- > 0;) {
    std::string Cond;
    raw_string_ostream C(Cond);
    renderPredicate(*Conds[I], C);
    C.flush();
    Expr = "(" + Cond + " ? " + Thens[I]->getName().str() + " : " + Expr + ")";
  }
  return Expr;
}

/// Emit the LLT constants and named type lists, in .td order.  Like a feature,
/// the record name is the C++ identifier; a type declared after one that uses
/// it would generate code that does not compile, which is why the names are
/// also checked for declaration order (see run()).
void LegalizerInfoEmitter::emitTypes(raw_ostream &OS) {
  std::vector<const Record *> Types =
      Records.getAllDerivedDefinitions("LegalizeNamedType");
  llvm::sort(Types, LessRecordByID());
  for (const Record *T : Types) {
    OS << "  const LLT " << T->getName() << " = ";
    if (T->isSubClassOf("LegalizeSelectType"))
      OS << selectText(*T, "Types", "Otherwise");
    else
      OS << T->getValueAsString("Expr");
    OS << ";\n";
  }
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
           A->isSubClassOf("LegalizeTypeTriple") ||
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
      Arg.isSubClassOf("LegalizeSelectLanes") ||
      Arg.isSubClassOf("LegalizeRawArg"))
    return "ShapeLanes";
  if (Arg.isSubClassOf("LegalizeSelectBits"))
    return "ShapeBits";
  if (Arg.isSubClassOf("LegalizeRawMutation"))
    return "ShapeRawMutation";
  if (Arg.isSubClassOf("LegalizeNamedType") ||
      Arg.isSubClassOf("LegalizeInlineType"))
    return "ShapeType";
  if (Arg.isSubClassOf("LegalizeTypePair"))
    return "ShapeTypePairList";
  if (Arg.isSubClassOf("LegalizeTypeTriple"))
    return "ShapeTypeTripleList";
  if (Arg.isSubClassOf("LegalizeMemDesc"))
    return "ShapeMemDescList";
  if (Arg.isSubClassOf("LegalizeNamedTypeList") ||
      Arg.isSubClassOf("LegalizeInlineTypeList") ||
      Arg.isSubClassOf("LegalizeSelectTypeList"))
    return "ShapeTypeList";
  // A bare LegalizeArgInt is deliberately not accepted: it is the class whose
  // three meanings (slot, bit width, lane count) the subclasses exist to tell
  // apart, so letting it through as any one of them would leave the ambiguity
  // this check is here to remove.
  return "";
}

/// The shapes a rule supplies, position by position.
///
/// A run of types, pairs, triples or memdesc tuples is one braced group in C++,
/// so it is compared as one position rather than element by element.
static std::vector<StringRef> suppliedShapes(const Record &Rule) {
  std::vector<const Record *> Args = Rule.getValueAsListOfDefs("Args");
  std::vector<StringRef> Supplied;
  if (isBracedGroup(Args)) {
    bool AllPairs = llvm::all_of(Args, [](const Record *A) {
      return A->isSubClassOf("LegalizeTypePair");
    });
    bool AllTriples = llvm::all_of(Args, [](const Record *A) {
      return A->isSubClassOf("LegalizeTypeTriple");
    });
    bool AllMemDesc = llvm::all_of(Args, [](const Record *A) {
      return A->isSubClassOf("LegalizeMemDesc");
    });
    Supplied.push_back(AllPairs     ? "ShapeTypePairList"
                       : AllTriples ? "ShapeTypeTripleList"
                       : AllMemDesc ? "ShapeMemDescList"
                                    : "ShapeType");
    return Supplied;
  }
  for (const Record *A : Args) {
    StringRef Got = shapeOf(*A);
    if (Got.empty())
      PrintFatalError(A, "unknown legalizer argument shape");
    Supplied.push_back(Got);
  }
  return Supplied;
}

/// The index in `Builder.Formals` that this rule's arguments match, or -1.
///
/// This is the key the lowering registry uses: a builder is not one entry but
/// one per overload, because the overloads differ in how many rows a rule
/// produces (`legalFor` takes four shapes, `legalForCartesianProduct` three).
static int matchedOverloadIndex(const Record &Rule) {
  const Record *B = Rule.getValueAsDef("Builder");
  std::vector<StringRef> Supplied = suppliedShapes(Rule);
  const ListInit *Overloads = B->getValueAsListInit("Formals");
  int Index = 0;
  for (const Init *O : Overloads->getElements()) {
    const auto *Inner = dyn_cast<ListInit>(O);
    if (Inner) {
      std::vector<StringRef> Names;
      for (const Init *E : Inner->getElements())
        if (const auto *DI = dyn_cast<DefInit>(E))
          Names.push_back(DI->getDef()->getName());
      if (Names == Supplied)
        return Index;
    }
    ++Index;
  }
  return -1;
}

/// The builder declares which guard overloads it has, and the guard the rule
/// carries has to be one of them.  Without this a rule could hand a cached
/// bool to a builder that takes a callable (or to one with no overload at all)
/// and the fragment would only fail when the target was compiled.
static void verifyGuardKind(const Record &Rule, StringRef Method,
                            const Record *B) {
  std::vector<const Record *> Guards = B->getValueAsListOfDefs("Guards");
  bool TakesBoolGuard = llvm::any_of(
      Guards, [](const Record *G) { return G->getName() == "GuardBool"; });
  bool TakesQueryGuard = llvm::any_of(
      Guards, [](const Record *G) { return G->getName() == "GuardQuery"; });
  const Record *P = Rule.getValueAsOptionalDef("Pred");
  if (!P)
    return;
  bool IsQuery = P->isSubClassOf("LegalizeQueryPredicateBase");
  if (IsQuery ? !TakesQueryGuard : !TakesBoolGuard) {
    StringRef What = Guards.empty() ? "no guard" : "a different kind of guard";
    PrintFatalError(Rule.getLoc(),
                    "legalizer builder '" + Method + "' takes " + What);
  }
}

/// The arguments have to line up with a signature the builder declares.
///
/// Formals is a list of overloads, so the rule matches if any one of them lines
/// up.  Args is a flat record list and a run of types becomes one braced group
/// in C++ (a type list, or a list of pairs/memdesc tuples), so a group is
/// compared as a whole position rather than element by element.
///
/// This is what stops `.clampScalar({s32})` (one argument, three declared) and
/// `.lower({s32})` (one argument, none declared) from being written at all:
/// both used to be emitted and only failed when the target was compiled.
static void verifyArgumentShape(const Record &Rule, StringRef Method,
                                const Record *B) {
  if (!Rule.getValueAsString("OpaqueArgs").empty())
    return;
  std::vector<StringRef> Supplied = suppliedShapes(Rule);

  // Formals is list<list<LegalizeArgShape>>, so each overload is itself a
  // ListInit; the inner elements are still records.
  const ListInit *Overloads = B->getValueAsListInit("Formals");
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

    if (Names == Supplied)
      return;
  }

  PrintFatalError(Rule.getLoc(), "legalizer builder '" + Method +
                                     "' does not take these arguments; "
                                     "declared " +
                                     Want);
}

/// The one place that decides whether a LegalizeRule record was written in the
/// typed core IR.  Consumers should not spell the schema class name: it is a
/// lowering detail, and a second spelling is exactly how a new consumer comes
/// to miss the core path.
enum class RuleKind { Legacy, Core };

static RuleKind classifyRule(const Record &Rule) {
  return Rule.isSubClassOf("LegalizeCoreRule") ? RuleKind::Core
                                               : RuleKind::Legacy;
}

static bool isCoreRule(const Record &Rule) {
  return classifyRule(Rule) == RuleKind::Core;
}

void LegalizerInfoEmitter::verifyRule(const Record &Rule) {
  if (isCoreRule(Rule)) {
    verifyCoreRule(Records, Rule);
    return;
  }
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

  verifyGuardKind(Rule, Method, B);

  verifyArgumentShape(Rule, Method, B);

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
  if (!Rule.getValueAsString("OpaqueArgs").empty() &&
      B->getValueAsListOfDefs("Guards").empty())
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
  if (Arg.isSubClassOf("LegalizeInlineType")) {
    OS << Arg.getValueAsString("Text");
    return;
  }
  if (Arg.isSubClassOf("LegalizeSelectTypeList")) {
    OS << selectText(Arg, "Lists", "Otherwise");
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
  if (Arg.isSubClassOf("LegalizeTypeTriple")) {
    OS << "{" << Arg.getValueAsDef("Type0")->getName() << ", "
       << Arg.getValueAsDef("Type1")->getName() << ", "
       << Arg.getValueAsDef("Type2")->getName() << "}";
    return;
  }
  if (Arg.isSubClassOf("LegalizeMemDesc")) {
    OS << "{" << Arg.getValueAsDef("Type0")->getName() << ", "
       << Arg.getValueAsDef("Type1")->getName() << ", "
       << Arg.getValueAsDef("MemTy")->getName() << ", "
       << Arg.getValueAsInt("Align") << "}";
    return;
  }
  if (Arg.isSubClassOf("LegalizeRawMutation")) {
    OS << Arg.getValueAsString("Text");
    return;
  }
  if (Arg.isSubClassOf("LegalizeSelectBits")) {
    std::vector<const Record *> Conds = Arg.getValueAsListOfDefs("Conds");
    std::vector<int64_t> Values = Arg.getValueAsListOfInts("Values");
    if (Conds.size() != Values.size())
      PrintFatalError(Arg.getLoc(),
                      "'" + Arg.getName() + "' has " +
                          std::to_string(Conds.size()) + " guards but " +
                          std::to_string(Values.size()) + " values");
    std::string Expr = std::to_string(Arg.getValueAsInt("Otherwise"));
    for (size_t I = Conds.size(); I-- > 0;) {
      std::string Cond;
      raw_string_ostream C(Cond);
      renderPredicate(*Conds[I], C);
      C.flush();
      Expr =
          "(" + Cond + " ? " + std::to_string(Values[I]) + " : " + Expr + ")";
    }
    OS << Expr;
    return;
  }
  if (Arg.isSubClassOf("LegalizeArgInt")) {
    StringRef Comment = Arg.getValueAsString("Comment");
    if (!Comment.empty())
      OS << "/*" << Comment << "=*/";
    OS << Arg.getValueAsInt("Value");
    return;
  }
  if (Arg.isSubClassOf("LegalizeSelectLanes")) {
    std::vector<const Record *> Conds = Arg.getValueAsListOfDefs("Conds");
    std::vector<int64_t> Values = Arg.getValueAsListOfInts("Values");
    if (Conds.size() != Values.size())
      PrintFatalError(Arg.getLoc(),
                      "'" + Arg.getName() + "' has " +
                          std::to_string(Conds.size()) + " guards but " +
                          std::to_string(Values.size()) + " values");
    std::string Expr = std::to_string(Arg.getValueAsInt("Otherwise"));
    for (size_t I = Conds.size(); I-- > 0;) {
      std::string Cond;
      raw_string_ostream C(Cond);
      renderPredicate(*Conds[I], C);
      C.flush();
      Expr = "(" + Cond + " ? " + std::to_string(Values[I]) + " : " + Expr + ")";
    }
    OS << Expr;
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
    if (isCoreRule(Rule)) {
      verifyCoreRule(Records, Rule);
      emitCoreRule(Rule, OS);
      OS << (I + 1 == E ? ";\n" : "\n");
      continue;
    }
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
  verifyLoweringRegistry(Records);

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

  for (const Record *SEL :
       Records.getAllDerivedDefinitions("LegalizeSelectType")) {
    for (const Record *T : SEL->getValueAsListOfDefs("Types"))
      if (T->getID() > SEL->getID())
        PrintFatalError(SEL->getLoc(),
                        "'" + T->getName() + "' is used by '" +
                            SEL->getName() +
                            "' but declared below it; the fragment is emitted "
                            "in .td order");
    const Record *O = SEL->getValueAsDef("Otherwise");
    if (O->getID() > SEL->getID())
      PrintFatalError(SEL->getLoc(),
                      "'" + O->getName() + "' is used by '" + SEL->getName() +
                          "' but declared below it; the fragment is emitted "
                          "in .td order");
  }

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
  // A core guard or a Select target names features as ordinary DefInit
  // leaves; the emitted fragment has to declare them just like a legacy
  // predicate dag does.
  for (const Record *R : Records.getAllDerivedDefinitions("LegalizeCoreRule")) {
    const Init *G = R->getValueInit("CoreGuard");
    if (G && !isa<UnsetInit>(G))
      collectFeatureLeaves(G, UsedFeatures);
    collectFeatureLeaves(R->getValueInit("CoreExpr"), UsedFeatures);
  }

  for (const Record *E : Records.getAllDerivedDefinitions("LegalizeOpaqueBool"))
    CollectUsed(E->getValueAsString("Expr"));
  // A raw query predicate is C++ text too: a feature named only inside it
  // still has to be declared, or the generated fragment does not compile.
  for (const Record *P :
       Records.getAllDerivedDefinitions("LegalizeQueryPredicate"))
    CollectUsed(P->getValueAsString("Expr"));
  for (const Record *P :
       Records.getAllDerivedDefinitions("LegalizeTypePredicate"))
    CollectUsed(P->getValueAsString("Call"));
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
       Records.getAllDerivedDefinitions("LegalizeSelectType"))
    for (const Record *Cond : X->getValueAsListOfDefs("Conds"))
      collectPredicateFeatures(Cond, UsedFeatures);
  for (const Record *X :
       Records.getAllDerivedDefinitions("LegalizeSelectTypeList"))
    for (const Record *Cond : X->getValueAsListOfDefs("Conds"))
      collectPredicateFeatures(Cond, UsedFeatures);
  for (const Record *X :
       Records.getAllDerivedDefinitions("LegalizeSelectLanes"))
    for (const Record *Cond : X->getValueAsListOfDefs("Conds"))
      collectPredicateFeatures(Cond, UsedFeatures);
  for (const Record *X : Records.getAllDerivedDefinitions("LegalizeRawArg"))
    CollectUsed(X->getValueAsString("Text"));
  for (const Record *X :
       Records.getAllDerivedDefinitions("LegalizeRawMutation"))
    CollectUsed(X->getValueAsString("Text"));

  // An inline `LegalizeSelectBits<...>` (or SelectType/SelectLanes) is an
  // anonymous record, so the getAllDerivedDefinitions() loops above do not
  // see it.  Walk the call arguments: a feature referenced only from one of
  // these selects must still be declared, or the generated fragment does not
  // compile.
  for (const Record *C : Records.getAllDerivedDefinitions("LegalizeCall"))
    for (const Record *A : C->getValueAsListOfDefs("Args")) {
      if (!A->isSubClassOf("LegalizeSelectBits") &&
          !A->isSubClassOf("LegalizeSelectType") &&
          !A->isSubClassOf("LegalizeSelectTypeList") &&
          !A->isSubClassOf("LegalizeSelectLanes"))
        continue;
      for (const Record *Cond : A->getValueAsListOfDefs("Conds"))
        collectPredicateFeatures(Cond, UsedFeatures);
    }

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

/// Split a canonical LLT spelling into its lane count and element part.
/// "v4s32" and "nxv4i32" both have four lanes; "s32" has one.  \p Scalable
/// reports the `nxv` prefix, whose total size depends on vscale.
unsigned splitLanes(StringRef S, StringRef &Elt, bool *Scalable = nullptr) {
  Elt = S;
  if (Scalable)
    *Scalable = false;
  StringRef T = S;
  if (T.consume_front("nxv")) {
    if (Scalable)
      *Scalable = true;
  } else if (!T.consume_front("v")) {
    return 1;
  }
  size_t L = 0;
  while (L < T.size() && isDigit(T[L]))
    ++L;
  if (L == 0)
    return 1;
  Elt = T.substr(L);
  return std::stoul(T.substr(0, L).str());
}

/// Bit width of a literal LLT spelling.  Returns 0 for spellings we cannot
/// size statically (pointer without a width, scalable vector, inline
/// expression).
unsigned lltBits(StringRef S) {
  StringRef Elt;
  bool Scalable = false;
  unsigned Lanes = splitLanes(S, Elt, &Scalable);
  // A scalable vector's total size is a multiple of vscale, so no static
  // width exists.  Saying 0 keeps a caller from baking in the minimum.
  if (Scalable)
    return 0;
  if (Elt.consume_front("p")) {
    size_t Colon = Elt.find(':');
    if (Colon == StringRef::npos)
      return 0;
    unsigned Bits = 0;
    // `p0:*` has no static width; it is bound by the Core IR configuration.
    if (Elt.substr(Colon + 1).getAsInteger(10, Bits))
      return 0;
    return Lanes * Bits;
  }
  size_t I = 0;
  while (I < Elt.size() && !isDigit(Elt[I]))
    ++I;
  if (I == Elt.size())
    return 0;
  return Lanes * std::stoul(Elt.substr(I).str());
}

/// Minimum bit width of a literal LLT spelling.  Unlike lltBits this is
/// defined for a scalable vector: `nxv16s8` has a minimum of 128 bits, and
/// that is the number `vscale x` multiplies.
unsigned lltMinBits(StringRef S) {
  StringRef Elt;
  unsigned Lanes = splitLanes(S, Elt);
  if (Elt.consume_front("p")) {
    size_t Colon = Elt.find(':');
    if (Colon == StringRef::npos)
      return 0;
    unsigned Bits = 0;
    if (Elt.substr(Colon + 1).getAsInteger(10, Bits))
      return 0;
    return Lanes * Bits;
  }
  size_t I = 0;
  while (I < Elt.size() && !isDigit(Elt[I]))
    ++I;
  if (I == Elt.size())
    return 0;
  return Lanes * std::stoul(Elt.substr(I).str());
}

/// Number of lanes of a literal LLT spelling.  A scalar has one.
unsigned lltLanes(StringRef S) {
  StringRef Elt;
  return splitLanes(S, Elt);
}

/// Element spelling of a literal LLT: "v4s32" -> "s32", "s32" -> "s32".
std::string lltElt(StringRef S) {
  StringRef Elt;
  splitLanes(S, Elt);
  return Elt.str();
}

/// A JSON string literal, quotes included.
std::string jsonEscaped(StringRef S) {
  std::string Buf;
  raw_string_ostream OS(Buf);
  jsonString(OS, S);
  OS.flush();
  return Buf;
}

void predicateJson(const RecordKeeper &Records, const Record &P,
                  std::string &Out);
std::string coreTypeValue(const RecordKeeper &Records, const Record &Ty);
std::string jsonStringValue(StringRef S);

/// Canonical Core IR spelling for a literal LLT expression, or "" when the
/// expression has no literal form.  A pointer whose width is computed from the
/// subtarget is still representable: `LLT::pointer(0, <expr>)` becomes `p0:*`
/// and the reader binds `*` to `config.pointer_width`.  That is the one symbol
/// the vocabulary allows, because the width is a property of the configuration,
/// not of the rule.
///
/// \p Records, when given, resolves a bare `LegalizeNamedType` name to the
/// expression it carries, so `X86_v2p0` and `LLT::fixed_vector(2, X86_p0)`
/// reach the same spelling.
std::string normalizeLlt(StringRef S, const RecordKeeper *Records = nullptr) {
  StringRef T = S.trim();
  if (T.empty())
    return "";
  if (Records && T.find_first_of("(){} \t\n") == StringRef::npos) {
    if (const Record *R = Records->getDef(T))
      if (R->isSubClassOf("LegalizeNamedType"))
        return normalizeLlt(R->getValueAsString("Expr"), Records);
  }
  {
    StringRef R = T;
    if (R.consume_front("LLT::scalar(") && R.consume_back(")") && !R.empty() &&
        llvm::all_of(R, isDigit))
      return "s" + R.str();
  }
  // Fixed and scalable vectors share a shape; only the lane prefix differs.
  // `nxv` is the canonical spelling of a scalable vector, so a rule that
  // depends on vscale is still literal in the Core IR.
  auto NormalizeVector = [&](StringRef Prefix, StringRef Head) -> std::string {
    StringRef R = T;
    if (!R.consume_front(Head) || !R.consume_back(")"))
      return "";
    size_t Comma = R.find(',');
    if (Comma == StringRef::npos)
      return "";
    StringRef L = R.substr(0, Comma).trim();
    StringRef E = R.substr(Comma + 1).trim();
    // The element is either a bare bit width or a nested LLT expression such
    // as `X86_p0` / `LLT::integer(8)`.
    std::string Elem =
        llvm::all_of(E, isDigit) ? "s" + E.str() : normalizeLlt(E, Records);
    if (L.empty() || Elem.empty() || !llvm::all_of(L, isDigit))
      return "";
    return Prefix.str() + L.str() + Elem;
  };
  if (std::string V = NormalizeVector("v", "LLT::fixed_vector("); !V.empty())
    return V;
  if (std::string V = NormalizeVector("nxv", "LLT::scalable_vector(");
      !V.empty())
    return V;
  // The extended kinds are distinct from `scalar` when
  // LLT::getUseExtended() is on, which this branch enables: `i32` is not
  // `s32`, and `f32` is not `s32` either.  Collapsing them would make the
  // Core IR unable to say which type a rule is about.
  {
    StringRef R = T;
    if (R.consume_front("LLT::integer(") && R.consume_back(")") && !R.empty() &&
        llvm::all_of(R, isDigit))
      return "i" + R.str();
  }
  {
    static const std::pair<StringRef, StringRef> Kinds[] = {
        {"LLT::bfloat16()", "bf16"},
        {"LLT::float16()", "f16"},
        {"LLT::float32()", "f32"},
        {"LLT::float64()", "f64"},
        {"LLT::float128()", "f128"}};
    for (const auto &K : Kinds)
      if (T == K.first)
        return K.second.str();
  }
  {
    StringRef R = T;
    if (R.consume_front("LLT::pointer(") && R.consume_back(")")) {
      size_t Comma = R.find(',');
      if (Comma != StringRef::npos) {
        StringRef AS = R.substr(0, Comma).trim();
        StringRef Bits = R.substr(Comma + 1).trim();
        if (!AS.empty() && !Bits.empty() && llvm::all_of(AS, isDigit)) {
          if (llvm::all_of(Bits, isDigit))
            return "p" + AS.str() + ":" + Bits.str();
          return "p" + AS.str() + ":*";
        }
      }
    }
  }
  return "";
}

/// Number of pointer lanes in a canonical LLT spelling, or 0 when it is not a
/// pointer type: "p0:64" and "p0:*" are one lane, "v2p0:*" is two.
unsigned lltPtrLanes(StringRef S) {
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
  if (!T.consume_front("p") || T.find(':') == StringRef::npos)
    return 0;
  return Lanes;
}

/// Core IR value for one type position.  A literal type is its canonical
/// spelling; a select type is a nested `select` node whose branches are values
/// as well.  The guards are never folded: the file is generated once and read
/// under many configurations.
std::string coreTypeValue(const RecordKeeper &Records, const Record &Ty) {
  if (Ty.isSubClassOf("LegalizeSelectType")) {
    std::vector<const Record *> Conds = Ty.getValueAsListOfDefs("Conds");
    std::vector<const Record *> Types = Ty.getValueAsListOfDefs("Types");
    if (Conds.size() != Types.size())
      PrintFatalError(Ty.getLoc(), "'" + Ty.getName() + "' has " +
                                       std::to_string(Conds.size()) +
                                       " guards but " +
                                       std::to_string(Types.size()) + " types");
    std::string Value = coreTypeValue(Records, *Ty.getValueAsDef("Otherwise"));
    if (Value.empty())
      return "";
    for (size_t I = Conds.size(); I-- > 0;) {
      std::string Cond;
      predicateJson(Records, *Conds[I], Cond);
      std::string Branch = coreTypeValue(Records, *Types[I]);
      if (Branch.empty())
        return "";
      Value = "{\"select\":{\"cond\":" + Cond + ",\"then\":" + Branch +
              ",\"else\":" + Value + "}}";
    }
    return Value;
  }
  if (!Ty.isSubClassOf("LegalizeNamedType"))
    return "";
  std::string Norm = normalizeLlt(Ty.getValueAsString("Expr"), &Records);
  if (Norm.empty())
    return "";
  return jsonStringValue(Norm);
}

/// Bit count of a type as a Core IR value.  A select type keeps its guards, so
/// `clampScalar` can compare against `Is64Bit ? 64 : 32` without choosing one.
std::string coreTypeBitsValue(const RecordKeeper &Records, const Record &Ty) {
  if (Ty.isSubClassOf("LegalizeSelectType")) {
    std::vector<const Record *> Conds = Ty.getValueAsListOfDefs("Conds");
    std::vector<const Record *> Types = Ty.getValueAsListOfDefs("Types");
    if (Conds.size() != Types.size())
      return "";
    std::string Value =
        coreTypeBitsValue(Records, *Ty.getValueAsDef("Otherwise"));
    if (Value.empty())
      return "";
    for (size_t I = Conds.size(); I-- > 0;) {
      std::string Cond;
      predicateJson(Records, *Conds[I], Cond);
      std::string Branch = coreTypeBitsValue(Records, *Types[I]);
      if (Branch.empty())
        return "";
      Value = "{\"select\":{\"cond\":" + Cond + ",\"then\":" + Branch +
              ",\"else\":" + Value + "}}";
    }
    return Value;
  }
  if (!Ty.isSubClassOf("LegalizeNamedType"))
    return "";
  std::string Norm = normalizeLlt(Ty.getValueAsString("Expr"), &Records);
  unsigned Bits = Norm.empty() ? 0 : lltBits(Norm);
  if (Bits == 0)
    return "";
  return jsonStringValue(std::to_string(Bits));
}

/// Lane count of a type as a Core IR value.
std::string coreTypeLanesValue(const RecordKeeper &Records, const Record &Ty) {
  if (Ty.isSubClassOf("LegalizeSelectType")) {
    std::vector<const Record *> Conds = Ty.getValueAsListOfDefs("Conds");
    std::vector<const Record *> Types = Ty.getValueAsListOfDefs("Types");
    if (Conds.size() != Types.size())
      return "";
    std::string Value =
        coreTypeLanesValue(Records, *Ty.getValueAsDef("Otherwise"));
    if (Value.empty())
      return "";
    for (size_t I = Conds.size(); I-- > 0;) {
      std::string Cond;
      predicateJson(Records, *Conds[I], Cond);
      std::string Branch = coreTypeLanesValue(Records, *Types[I]);
      if (Branch.empty())
        return "";
      Value = "{\"select\":{\"cond\":" + Cond + ",\"then\":" + Branch +
              ",\"else\":" + Value + "}}";
    }
    return Value;
  }
  if (!Ty.isSubClassOf("LegalizeNamedType"))
    return "";
  std::string Norm = normalizeLlt(Ty.getValueAsString("Expr"), &Records);
  if (Norm.empty())
    return "";
  return std::to_string(lltLanes(Norm));
}

/// Core IR value for a lane count: a number or a select over numbers.
std::string coreLanesValue(const RecordKeeper &Records, const Record &Arg) {
  if (Arg.isSubClassOf("LegalizeArgLanes"))
    return std::to_string(Arg.getValueAsInt("Value"));
  if (!Arg.isSubClassOf("LegalizeSelectLanes"))
    return "";
  std::vector<const Record *> Conds = Arg.getValueAsListOfDefs("Conds");
  std::vector<int64_t> Values = Arg.getValueAsListOfInts("Values");
  if (Conds.size() != Values.size())
    PrintFatalError(Arg.getLoc(), "'" + Arg.getName() + "' has " +
                                      std::to_string(Conds.size()) +
                                      " guards but " +
                                      std::to_string(Values.size()) + " values");
  std::string Value = std::to_string(Arg.getValueAsInt("Otherwise"));
  for (size_t I = Conds.size(); I-- > 0;) {
    std::string Cond;
    predicateJson(Records, *Conds[I], Cond);
    Value = "{\"select\":{\"cond\":" + Cond + ",\"then\":" +
            std::to_string(Values[I]) + ",\"else\":" + Value + "}}";
  }
  return Value;
}

/// Core IR value for a type list: an array or a select over arrays.
std::string coreTypeListValue(const RecordKeeper &Records,
                              const Record &List) {
  if (List.isSubClassOf("LegalizeSelectTypeList")) {
    std::vector<const Record *> Conds = List.getValueAsListOfDefs("Conds");
    std::vector<const Record *> Lists = List.getValueAsListOfDefs("Lists");
    if (Conds.size() != Lists.size())
      PrintFatalError(List.getLoc(), "'" + List.getName() + "' has " +
                                         std::to_string(Conds.size()) +
                                         " guards but " +
                                         std::to_string(Lists.size()) +
                                         " lists");
    std::string Value =
        coreTypeListValue(Records, *List.getValueAsDef("Otherwise"));
    if (Value.empty())
      return "";
    for (size_t I = Conds.size(); I-- > 0;) {
      std::string Cond;
      predicateJson(Records, *Conds[I], Cond);
      std::string Branch = coreTypeListValue(Records, *Lists[I]);
      if (Branch.empty())
        return "";
      Value = "{\"select\":{\"cond\":" + Cond + ",\"then\":" + Branch +
              ",\"else\":" + Value + "}}";
    }
    return Value;
  }
  if (!List.isSubClassOf("LegalizeNamedTypeList") &&
      !List.isSubClassOf("LegalizeInlineTypeList"))
    return "";
  std::string S = "[";
  StringRef Sep;
  for (const Record *E : List.getValueAsListOfDefs("Elems")) {
    std::string V = coreTypeValue(Records, *E);
    if (V.empty())
      return "";
    S += Sep.str();
    S += V;
    Sep = ",";
  }
  return S + "]";
}
/// True when a list argument cannot be spelled as one literal list: either it
/// chooses between lists, or one of its elements is a select type.  Such a list
/// keeps a single `typeIn` over the conditional value; a literal list instead
/// expands to one `typeIs` per element so rule positions stay put.
static bool isSelectBearingList(const Record &List) {
  if (List.isSubClassOf("LegalizeSelectTypeList"))
    return true;
  if (!List.isSubClassOf("LegalizeNamedTypeList") &&
      !List.isSubClassOf("LegalizeInlineTypeList"))
    return false;
  return llvm::any_of(List.getValueAsListOfDefs("Elems"),
                      [](const Record *E) {
                        return E->isSubClassOf("LegalizeSelectType");
                      });
}

std::string jsonStringValue(StringRef S) { return "\"" + S.str() + "\""; }

/// typeIs with an already-built JSON value: a quoted spelling for a literal
/// type, or a select object when the type is chosen from feature guards.
std::string typeIsValue(unsigned Idx, StringRef TyJson) {
  return "{\"kind\":\"leaf\",\"name\":\"typeIs\",\"args\":[" +
         std::to_string(Idx) + "," + TyJson.str() + "]}";
}

/// typeIn with a list value (or a select over lists).
std::string typeInValue(unsigned Idx, StringRef ListJson) {
  return "{\"kind\":\"leaf\",\"name\":\"typeIn\",\"args\":[" +
         std::to_string(Idx) + "," + ListJson.str() + "]}";
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

/// memSizePtr(mem_idx, lanes, min_align_bits): the MMO at mem_idx is exactly
/// `lanes` pointers wide.  The width is `config.pointer_width`, so a memdesc
/// whose memory type is a pointer stays literal without baking in 32 or 64.
std::string memSizePtr(unsigned Idx, unsigned Lanes, unsigned MinAlignBits) {
  return "{\"kind\":\"leaf\",\"name\":\"memSizePtr\",\"args\":[" +
         std::to_string(Idx) + "," + std::to_string(Lanes) + "," +
         std::to_string(MinAlignBits) + "]}";
}

/// memSizeScalable(mem_idx, min_bits, min_align_bits): the MMO at mem_idx has
/// size `vscale x min_bits`.  A scalable memory type has no static bit size,
/// and TypePairAndMemDesc::isCompatible compares TypeSize, so the vscale
/// factor has to be part of the predicate rather than folded into an integer.
std::string memSizeScalable(unsigned Idx, unsigned MinBits,
                            unsigned MinAlignBits) {
  return "{\"kind\":\"leaf\",\"name\":\"memSizeScalable\",\"args\":[" +
         std::to_string(Idx) + "," + std::to_string(MinBits) + "," +
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

std::string scalarNarrowerThanValue(unsigned Idx, StringRef BitsJson) {
  return "{\"kind\":\"leaf\",\"name\":\"scalarNarrowerThan\",\"args\":[" +
         std::to_string(Idx) + "," + BitsJson.str() + "]}";
}

std::string scalarNarrowerThan(unsigned Idx, StringRef Bits) {
  return scalarNarrowerThanValue(Idx, jsonStringValue(Bits));
}

std::string anyOf(const std::vector<std::string> &Parts) {
  std::string S = "{\"kind\":\"nary\",\"op\":\"any_of\",\"args\":[";
  for (size_t I = 0; I != Parts.size(); ++I) {
    if (I)
      S += ",";
    S += Parts[I];
  }
  return S + "]}";
}

std::string scalarOrEltNarrowerThanValue(unsigned Idx, StringRef BitsJson) {
  return "{\"kind\":\"leaf\",\"name\":\"scalarOrEltNarrowerThan\",\"args\":[" +
         std::to_string(Idx) + "," + BitsJson.str() + "]}";
}

std::string scalarOrEltNarrowerThan(unsigned Idx, StringRef Bits) {
  return scalarOrEltNarrowerThanValue(Idx, jsonStringValue(Bits));
}

std::string scalarWiderThanValue(unsigned Idx, StringRef BitsJson) {
  return "{\"kind\":\"leaf\",\"name\":\"scalarWiderThan\",\"args\":[" +
         std::to_string(Idx) + "," + BitsJson.str() + "]}";
}

std::string scalarWiderThan(unsigned Idx, StringRef Bits) {
  return scalarWiderThanValue(Idx, jsonStringValue(Bits));
}

std::string oneArgLeaf(StringRef Name, unsigned A) {
  return "{\"kind\":\"leaf\",\"name\":\"" + Name.str() + "\",\"args\":[" +
         std::to_string(A) + "]}";
}

std::string isVector(unsigned Idx) { return oneArgLeaf("isVector", Idx); }

std::string memSizeNotPow2(unsigned Idx) {
  return oneArgLeaf("memSizeNotPow2", Idx);
}

std::string memSizeNotByteSizePow2(unsigned Idx) {
  return oneArgLeaf("memSizeNotByteSizePow2", Idx);
}

std::string sizeNotPow2(unsigned Idx) { return oneArgLeaf("sizeNotPow2", Idx); }

std::string scalarOrEltSizeNotPow2(unsigned Idx) {
  return oneArgLeaf("scalarOrEltSizeNotPow2", Idx);
}

std::string numElementsNotPow2(unsigned Idx) {
  return oneArgLeaf("numElementsNotPow2", Idx);
}

std::string fewerElementsThanValue(unsigned Idx, StringRef LanesJson) {
  return "{\"kind\":\"leaf\",\"name\":\"fewerElementsThan\",\"args\":[" +
         std::to_string(Idx) + "," + LanesJson.str() + "]}";
}

std::string moreElementsThanValue(unsigned Idx, StringRef LanesJson) {
  return "{\"kind\":\"leaf\",\"name\":\"moreElementsThan\",\"args\":[" +
         std::to_string(Idx) + "," + LanesJson.str() + "]}";
}

std::string elementTypeIsValue(unsigned Idx, StringRef TyJson) {
  return "{\"kind\":\"leaf\",\"name\":\"elementTypeIs\",\"args\":[" +
         std::to_string(Idx) + "," + TyJson.str() + "]}";
}

/// Types[Other].getScalarSizeInBits() > Types[Idx].getScalarSizeInBits()
std::string scalarSizeLargerThanSlot(unsigned Other, unsigned Idx) {
  return "{\"kind\":\"leaf\",\"name\":\"scalarSizeLargerThanSlot\","
         "\"args\":[" +
         std::to_string(Other) + "," + std::to_string(Idx) + "]}";
}

/// Types[Other].getScalarSizeInBits() < Types[Idx].getScalarSizeInBits()
std::string scalarSizeSmallerThanSlot(unsigned Other, unsigned Idx) {
  return "{\"kind\":\"leaf\",\"name\":\"scalarSizeSmallerThanSlot\","
         "\"args\":[" +
         std::to_string(Other) + "," + std::to_string(Idx) + "]}";
}

/// A LegalizeTypePredicate carries a C++ call such as `typeIs(0, X86_s32)`.
/// Its Core IR form is the same leaf vocabulary the expansion uses, so the call
/// has to be translated rather than passed through: the Core IR names types in
/// the normalized `s32` / `v4s32` form, not by the target's record names.
///
/// Only the shapes the evaluator already understands are accepted.  Anything
/// else is an error rather than an opaque leaf, because a rule that reaches
/// this point has claimed to be expressible.
std::string typePredicateJson(const RecordKeeper &Records, const Record &R) {
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
    PrintFatalError(R.getLoc(), "type predicate '" + Call + "' is not a call");

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
                      "\"args\":[" +
                      std::to_string(Idx0) + ",[";
    StringRef Sep;
    for (StringRef E : Elems) {
      E = E.trim();
      if (E.empty())
        continue;
      // An element may be a named type (including a select type, whose Core
      // IR form is the conditional value) or a literal LLT spelling.
      std::string Value;
      if (const Record *T = Records.getDef(E))
        if (T->isSubClassOf("LegalizeNamedType"))
          Value = coreTypeValue(Records, *T);
      if (Value.empty()) {
        std::string Norm = normalizeLlt(E, &Records);
        if (!Norm.empty())
          Value = jsonStringValue(Norm);
      }
      if (Value.empty())
        PrintFatalError(R.getLoc(), "typeInSet names a type with no literal "
                                    "Core IR form: " +
                                        E);
      Out += Sep.str();
      Out += Value;
      Sep = ",";
    }
    return Out + "]]}";
  }

  SmallVector<StringRef, 4> Args;
  SplitArgs(Rest, Args);

  auto Idx = [&](size_t I) {
    unsigned V = 0;
    if (I >= Args.size() || Args[I].getAsInteger(10, V))
      PrintFatalError(R.getLoc(), "type predicate '" + Call +
                                      "' needs an index at " +
                                      std::to_string(I));
    return V;
  };
  // A type argument is either a normalized LLT or the name of a
  // LegalizeNamedType record; the Core IR has to see the same type either way.
  // A named record is resolved through the record it names, and a select type
  // becomes the conditional value rather than the type of one configuration.
  auto Ty = [&](size_t I) -> std::string {
    if (I >= Args.size())
      PrintFatalError(R.getLoc(),
                      "type predicate '" + Call + "' is missing a type");
    if (const Record *T = Records.getDef(Args[I]))
      if (T->isSubClassOf("LegalizeNamedType")) {
        std::string V = coreTypeValue(Records, *T);
        if (!V.empty())
          return V;
      }
    std::string Norm = normalizeLlt(Args[I], &Records);
    if (!Norm.empty())
      return jsonStringValue(Norm);
    PrintFatalError(R.getLoc(), "type predicate '" + Call +
                                    "' names a type that has no "
                                    "literal Core IR form: " +
                                    Args[I]);
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
    return typeIsValue(Idx(0), Ty(1));
  if (Name == "memTypeIsNot")
    return "{\"kind\":\"leaf\",\"name\":\"memTypeIsNot\",\"args\":[" +
           std::to_string(Idx(0)) + "," + std::to_string(Idx(1)) + "]}";
  if (Name == "isVector")
    return isVector(Idx(0));
  if (Name == "scalarNarrowerThan")
    return scalarNarrowerThan(Idx(0), Bits(1));
  if (Name == "scalarWiderThan")
    return scalarWiderThan(Idx(0), Bits(1));
  if (Name == "sizeIs")
    return sizeIs(Idx(0), Bits(1));
  PrintFatalError(R.getLoc(), "type predicate '" + Name +
                                  "' has no Core IR expansion; "
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
      // A quoted string is the shape a misspelled record name takes, so name
      // it rather than reporting a generic unsupported node.
      if (const auto *SI = dyn_cast<StringInit>(&Node))
        PrintFatalError(P.getLoc(), "predicate dag leaf '" +
                                        SI->getAsString() +
                                        "' is not a record");
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
  }
  if (P.isSubClassOf("LegalizeTypePredicate")) {
    // A named type predicate standing alone is the same leaf a dag would
    // carry; only its position in the tree differs.
    Out = typePredicateJson(Records, P);
    return;
  }
  if (P.isSubClassOf("LegalizeOpaqueBool") ||
      P.isSubClassOf("LegalizeQueryPredicate")) {
    // Escape: raw C++.  It has no Core IR form by construction.
    Out = "{\"kind\":\"leaf\",\"name\":\"opaque\",\"args\":[\"" +
          P.getName().str() + "\"]}";
    return;
  }
  PrintFatalError(&P, "unknown LegalizePredicate record");
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
  std::string Mutation; // empty means null
  /// 0.4's operator view.  Empty on a legacy row; a rule that opts into the
  /// primitive vocabulary emits all three, and the 0.3 fields above are its
  /// projection so that both readers see one lowering entry.
  std::string Op;
  std::string On;
  std::string To;
};

std::string mutationJsonValue(unsigned Target, StringRef Kind,
                              StringRef ValueJson) {
  return "{\"target\":" + std::to_string(Target) + ",\"kind\":\"" + Kind.str() +
         "\",\"value\":" +
         (ValueJson.empty() ? std::string("null") : ValueJson.str()) + "}";
}

std::string mutationJson(unsigned Target, StringRef Kind, StringRef Value) {
  return mutationJsonValue(Target, Kind,
                           Value.empty() ? std::string()
                                         : jsonStringValue(Value));
}

std::string mutationJsonInt(unsigned Target, StringRef Kind, int Value) {
  return "{\"target\":" + std::to_string(Target) + ",\"kind\":\"" + Kind.str() +
         "\",\"value\":" + std::to_string(Value) + "}";
}

//===----------------------------------------------------------------------===//
// Sort-checked core IR
//
// A core rule is a dag: one lattice operator, an observation expression and a
// target constructor.  This block parses that dag, assigns a sort to every
// node, checks the (operator, observation, target) triple against a total
// action table, serializes the node tree into Core IR, and translates it back
// to a C++ shim call.  The parse is not a second grammar: TableGen already
// parsed the dag, and a def that is not part of the vocabulary is rejected
// here with the rule's source location.
//===----------------------------------------------------------------------===//

struct CoreNode {
  enum NodeKind { OpNode, IntNode, StringNode, DefNode } Kind = OpNode;
  /// The operator/observer/constructor def, or the named-type/set/tag def.
  const Record *Op = nullptr;
  int IntValue = 0;
  std::string StringValue;
  SmallVector<std::unique_ptr<CoreNode>, 2> Kids;
};

/// The stable name a core node carries in the serialized IR (e.g. "elemOf").
/// Dispatch on this field, not on the TableGen record name: the schema's Name
/// is the contract, and diagnostics can still use Record::getName().
static StringRef coreTag(const Record &R) { return R.getValueAsString("Name"); }

static std::unique_ptr<CoreNode> parseCoreNode(const Init &I,
                                               const Record &Owner) {
  auto N = std::make_unique<CoreNode>();
  if (const auto *II = dyn_cast<IntInit>(&I)) {
    N->Kind = CoreNode::IntNode;
    N->IntValue = II->getValue();
    return N;
  }
  if (const auto *SI = dyn_cast<StringInit>(&I)) {
    N->Kind = CoreNode::StringNode;
    N->StringValue = SI->getValue().str();
    return N;
  }
  if (const auto *DI = dyn_cast<DefInit>(&I)) {
    N->Kind = CoreNode::DefNode;
    N->Op = DI->getDef();
    return N;
  }
  if (const auto *D = dyn_cast<DagInit>(&I)) {
    const auto *OD = dyn_cast<DefInit>(D->getOperator());
    if (!OD)
      PrintFatalError(Owner.getLoc(),
                      "core rule: dag operator is not a record");
    N->Op = OD->getDef();
    for (unsigned A = 0, E = D->getNumArgs(); A != E; ++A) {
      if (D->getArgName(A))
        PrintFatalError(Owner.getLoc(),
                        "core rule: named dag arguments are not supported");
      N->Kids.push_back(parseCoreNode(*D->getArg(A), Owner));
    }
    return N;
  }
  PrintFatalError(Owner.getLoc(),
                  "core rule: unsupported operand '" + I.getAsString() + "'");
  return N;
}

static bool coreSortAccepts(const Record *Expected, const Record *Actual) {
  if (Expected == Actual)
    return true;
  if (Expected->isSubClassOf("LegalizeSortSet"))
    return llvm::is_contained(Expected->getValueAsListOfDefs("Members"),
                              Actual);
  return false;
}

static std::string coreSortListText(ArrayRef<const Record *> Sorts) {
  std::string Out;
  for (size_t I = 0; I != Sorts.size(); ++I) {
    if (I)
      Out += " or ";
    Out += Sorts[I]->getValueAsString("Name").str();
  }
  return Out;
}

static int64_t coreIntLiteral(const CoreNode &N, const Record &Owner,
                              StringRef What);

/// The sort of one node, after checking every operand against its declared
/// signature.  `Owner` is the rule, so every diagnostic points at the .td line.
static const Record *coreSortOf(const CoreNode &N, const RecordKeeper &Records,
                                const Record &Owner) {
  switch (N.Kind) {
  case CoreNode::IntNode:
    return Records.getDef("SortInt");
  case CoreNode::StringNode:
    return Records.getDef("SortString");
  case CoreNode::DefNode: {
    const Record *R = N.Op;
    if (R->isSubClassOf("LegalizeNamedType"))
      return Records.getDef("SortType");
    if (R->isSubClassOf("LegalizeCoreSet"))
      return R->getValueAsDef("ElemSort");
    if (R->isSubClassOf("LegalizeCoreExpr"))
      return R->getValueAsDef("Result");
    // A cached per-subtarget bool is already a Bool leaf; a rule may name the
    // existing LegalizeFeature record instead of spelling (Feature "...").
    if (R->isSubClassOf("LegalizeFeature"))
      return Records.getDef("SortBool");
    PrintFatalError(Owner.getLoc(),
                    "core rule: '" + R->getName() + "' is not a core node");
  }
  case CoreNode::OpNode:
    break;
  }

  const Record *Op = N.Op;
  if (Op->isSubClassOf("LegalizeCoreOperator"))
    PrintFatalError(Owner.getLoc(), "core rule: operator '" + Op->getName() +
                                        "' cannot be used as a value");
  if (!Op->isSubClassOf("LegalizeCoreExpr"))
    PrintFatalError(Owner.getLoc(), "core rule: '" + Op->getName() +
                                        "' is not a core operator, "
                                        "observer, constructor, or combinator");

  StringRef Name = Op->getName();
  StringRef Tag = coreTag(*Op);
  const std::vector<const Record *> ArgSorts =
      Op->getValueAsListOfDefs("ArgSorts");

  // The set-membership and comparison nodes are polymorphic; their operand
  // sorts are stated as constraints rather than a fixed signature.
  if (Op->getValueAsBit("SetMembership")) {
    if (N.Kids.size() != 2)
      PrintFatalError(Owner.getLoc(), "core rule: '" + Name +
                                          "' expects 2 operands, got " +
                                          std::to_string(N.Kids.size()));
    const Record *Lhs = coreSortOf(*N.Kids[0], Records, Owner);
    const CoreNode &RhsNode = *N.Kids[1];
    if (RhsNode.Kind == CoreNode::DefNode &&
        RhsNode.Op->isSubClassOf("LegalizeCoreSet")) {
      const Record *Elem = RhsNode.Op->getValueAsDef("ElemSort");
      if (Lhs != Elem)
        PrintFatalError(Owner.getLoc(), "core rule: 'in' expects a " +
                                            Elem->getValueAsString("Name") +
                                            " on the left, got " +
                                            Lhs->getValueAsString("Name"));
      return Op->getValueAsDef("Result");
    }
    // A Clamp is an integer range, so `(in x, (clamp lo, hi))` is membership
    // in an interval rather than in an enumerated set.
    if (RhsNode.Kind == CoreNode::OpNode &&
        coreTag(*RhsNode.Op) == "clamp") {
      if (Lhs != Records.getDef("SortInt"))
        PrintFatalError(Owner.getLoc(),
                        "core rule: a Clamp range can only be tested against "
                        "an Int, got " +
                            Lhs->getValueAsString("Name"));
      if (RhsNode.Kids.size() != 2)
        PrintFatalError(Owner.getLoc(),
                        "core rule: 'Clamp' expects 2 operands");
      for (const auto &K : RhsNode.Kids)
        if (coreSortOf(*K, Records, Owner) != Records.getDef("SortInt"))
          PrintFatalError(Owner.getLoc(),
                          "core rule: 'Clamp' operands must be Int");
      coreIntLiteral(*RhsNode.Kids[0], Owner, "'Clamp' lower bound");
      coreIntLiteral(*RhsNode.Kids[1], Owner, "'Clamp' upper bound");
      return Op->getValueAsDef("Result");
    }
    PrintFatalError(Owner.getLoc(),
                    "core rule: the right operand of 'in' is not a set");
  }

  if (Op->getValueAsBit("SameSortOperands")) {
    if (N.Kids.size() != 2)
      PrintFatalError(Owner.getLoc(), "core rule: '" + Name +
                                          "' expects 2 operands, got " +
                                          std::to_string(N.Kids.size()));
    const Record *L = coreSortOf(*N.Kids[0], Records, Owner);
    const Record *R = coreSortOf(*N.Kids[1], Records, Owner);
    if (L != R)
      PrintFatalError(Owner.getLoc(),
                      "core rule: '" + Name + "' compares " +
                          L->getValueAsString("Name") + " with " +
                          R->getValueAsString("Name") +
                          "; both operands must have the same sort");
    static constexpr StringLiteral Comparable[] = {"Int", "Ordering", "Kind",
                                                   "Type"};
    StringRef SortName = L->getValueAsString("Name");
    if (!llvm::is_contained(Comparable, SortName))
      PrintFatalError(Owner.getLoc(), "core rule: '" + Name +
                                          "' cannot compare values of sort " +
                                          SortName);
    // LLT has equality but no total order, and a Kind is a tag rather than a
    // number.  Before this check `lt` over Kind emitted `isVector()` (i.e.
    // `==`) and `lt` over Type emitted `LLT < LLT`, which does not compile.
    StringRef OpName = coreTag(*Op);
    if ((SortName == "Kind" || SortName == "Type") && OpName != "eq" &&
        OpName != "ne")
      PrintFatalError(Owner.getLoc(),
                      "core rule: '" + Name + "' cannot order values of sort " +
                          SortName + "; only eq/ne are defined");
    return Op->getValueAsDef("Result");
  }

  const bool Variadic = Op->getValueAsBit("Variadic");
  if (Variadic) {
    if (N.Kids.empty() || ArgSorts.size() != 1)
      PrintFatalError(Owner.getLoc(),
                      "core rule: '" + Name + "' expects at least 1 operand");
    for (const auto &K : N.Kids) {
      const Record *Got = coreSortOf(*K, Records, Owner);
      if (!coreSortAccepts(ArgSorts[0], Got))
        PrintFatalError(Owner.getLoc(),
                        "core rule: '" + Name + "' expects operands of sort " +
                            ArgSorts[0]->getValueAsString("Name") + ", got " +
                            Got->getValueAsString("Name"));
    }
    return Op->getValueAsDef("Result");
  }

  if (N.Kids.size() != ArgSorts.size())
    PrintFatalError(Owner.getLoc(), "core rule: '" + Name + "' expects " +
                                        std::to_string(ArgSorts.size()) +
                                        " operand(s), got " +
                                        std::to_string(N.Kids.size()));
  for (size_t I = 0; I != N.Kids.size(); ++I) {
    const Record *Got = coreSortOf(*N.Kids[I], Records, Owner);
    if (!coreSortAccepts(ArgSorts[I], Got))
      PrintFatalError(Owner.getLoc(),
                      "core rule: '" + Name + "' operand " + std::to_string(I) +
                          " expects sort " +
                          ArgSorts[I]->getValueAsString("Name") + ", got " +
                          Got->getValueAsString("Name"));
  }

  // Payload checks the sort signature cannot express: these operators carry
  // literal numbers, a type name, or a feature condition, not a general
  // expression of that sort.  The literal checks also reject negative slot
  // indices: the shims index Query.Types and Query.MMODescrs directly.
  if (Tag == "slot" || Tag == "memSlot" || Tag == "sameAs")
    coreIntLiteral(*N.Kids[0], Owner, ("'" + Name + "' index").str());
  if (Tag == "pow2Min" || Tag == "multipleOf" || Tag == "minVectorSize")
    coreIntLiteral(*N.Kids[0], Owner, ("'" + Name + "' operand").str());
  if (Tag == "clamp") {
    coreIntLiteral(*N.Kids[0], Owner, "'Clamp' lower bound");
    coreIntLiteral(*N.Kids[1], Owner, "'Clamp' upper bound");
  }
  if (Tag == "select") {
    coreIntLiteral(*N.Kids[1], Owner, "'Select' true branch");
    coreIntLiteral(*N.Kids[2], Owner, "'Select' false branch");
  }
  if (Tag == "feature" && N.Kids[0]->Kind != CoreNode::StringNode)
    PrintFatalError(Owner.getLoc(),
                    "core rule: 'Feature' expects a string condition");
  if (Tag == "ty" && (N.Kids[0]->Kind != CoreNode::DefNode ||
                      !N.Kids[0]->Op->isSubClassOf("LegalizeNamedType")))
    PrintFatalError(Owner.getLoc(),
                    "core rule: 'Ty' expects a LegalizeNamedType record");

  return Op->getValueAsDef("Result");
}

struct CoreActionRow {
  StringRef Operator;
  StringRef TargetSort;
  StringRef Action;
};

/// C5: the action is a total function of (operator, target sort).  The
/// observation chooses the guard; the target chooses the mutation payload.
static const CoreActionRow CoreActionTable[] = {
    {"widen", "Type", "WidenScalar"},
    {"widen", "Int", "WidenScalar"},
    {"narrow", "Type", "NarrowScalar"},
    {"narrow", "Int", "NarrowScalar"},
    {"more", "Int", "MoreElements"},
    {"fewer", "Int", "FewerElements"},
    {"fewer", "Type", "FewerElements"},
    {"bitcast", "Type", "Bitcast"},
    // A terminal operator has no target, so the empty sort keys its action.
    {"legal", "", "Legal"},
    {"custom", "", "Custom"},
    {"lower", "", "Lower"},
    {"unsupported", "", "Unsupported"},
    {"scalarize", "", "FewerElements"},
};

static const Record *findCoreAction(const RecordKeeper &Records,
                                    StringRef Name) {
  for (const Record *A : Records.getAllDerivedDefinitions("LegalizeAction"))
    if (A->getValueAsString("Name") == Name)
      return A;
  return nullptr;
}

static const CoreActionRow *lookupCoreAction(StringRef Operator,
                                             StringRef TargetSort) {
  for (const CoreActionRow &Row : CoreActionTable)
    if (Row.Operator == Operator && Row.TargetSort == TargetSort)
      return &Row;
  return nullptr;
}

/// A non-negative integer literal, or a diagnostic naming what was expected.
/// Several shims pass an operand straight into `Query.Types[...]` or into a
/// generated C++ expression; a non-literal there used to default to zero.
static int64_t coreIntLiteral(const CoreNode &N, const Record &Owner,
                              StringRef What) {
  if (N.Kind != CoreNode::IntNode)
    PrintFatalError(Owner.getLoc(),
                    "core rule: " + What + " must be an integer literal");
  if (N.IntValue < 0)
    PrintFatalError(Owner.getLoc(),
                    "core rule: " + What + " must not be negative");
  return N.IntValue;
}

/// The type slot an observation projects to.  A shim takes an unsigned
/// TypeIdx, so a missing projection is an error, not a default of zero: the
/// old fall-through silently turned `(ElemOf (ElemOf (Slot 1)))` into slot 0.
static unsigned coreTypeIdxOf(const CoreNode &N, const Record &Owner) {
  if (N.Kind == CoreNode::OpNode && coreTag(*N.Op) == "elemOf") {
    if (N.Kids.size() != 1)
      PrintFatalError(Owner.getLoc(), "core rule: 'ElemOf' expects 1 operand");
    const CoreNode &Inner = *N.Kids[0];
    if (Inner.Kind == CoreNode::OpNode && coreTag(*Inner.Op) == "elemOf")
      PrintFatalError(Owner.getLoc(),
                      "core rule: nested 'ElemOf' has no type-index "
                      "projection; a shim observes a slot or one element of a "
                      "slot");
    return coreTypeIdxOf(Inner, Owner);
  }
  if (N.Kind == CoreNode::IntNode)
    return static_cast<unsigned>(coreIntLiteral(N, Owner, "type slot index"));
  if (N.Kind == CoreNode::OpNode && coreTag(*N.Op) == "slot") {
    if (N.Kids.size() != 1)
      PrintFatalError(Owner.getLoc(), "core rule: 'Slot' expects 1 operand");
    return static_cast<unsigned>(
        coreIntLiteral(*N.Kids[0], Owner, "type slot index"));
  }
  PrintFatalError(Owner.getLoc(),
                  "core rule: the observation cannot be projected to a type "
                  "slot; expected a slot or one element of a slot");
  return 0;
}

/// One core rule, checked.  `Action` is derived, never written by the rule.
struct CheckedCoreRule {
  /// The parsed tree owns every node; Obs/Target point into it.
  std::unique_ptr<CoreNode> Root;
  const Record *Operator = nullptr;
  const Record *Action = nullptr;
  unsigned TypeIdx = 0;
  const CoreNode *Obs = nullptr;
  const CoreNode *Target = nullptr;
};

static CheckedCoreRule checkCoreRule(const Record &Rule,
                                     const RecordKeeper &Records) {
  const Init *E = Rule.getValueInit("CoreExpr");
  const auto *D = dyn_cast<DagInit>(E);
  if (!D)
    PrintFatalError(Rule.getLoc(), "core rule: CoreExpr must be a dag");
  std::unique_ptr<CoreNode> Root = parseCoreNode(*D, Rule);
  if (Root->Kind != CoreNode::OpNode ||
      !Root->Op->isSubClassOf("LegalizeCoreOperator"))
    PrintFatalError(Rule.getLoc(),
                    "core rule: the rule body is not a lattice operator");
  // A terminal operator has no target; four of them also have no
  // observation, so their rule is `(Op)` and the guard is the whole rule.
  const bool HasObs = Root->Op->getValueAsBit("HasObservation");
  const std::vector<const Record *> Targets =
      Root->Op->getValueAsListOfDefs("Targets");
  const bool HasTarget = !Targets.empty();
  if (Root->Kids.size() != (HasObs ? 1u : 0u) + (HasTarget ? 1u : 0u))
    PrintFatalError(
        Rule.getLoc(),
        "core rule: operator '" + Root->Op->getName() + "' expects " +
            std::to_string((HasObs ? 1 : 0) + (HasTarget ? 1 : 0)) +
            " operand(s), got " + std::to_string(Root->Kids.size()));

  if (HasObs) {
    const Record *Domain = Root->Op->getValueAsDef("Domain");
    const Record *OnSort = coreSortOf(*Root->Kids[0], Records, Rule);
    if (OnSort != Domain)
      PrintFatalError(Rule.getLoc(),
                      "core rule: operator '" + Root->Op->getName() +
                          "' expects an observation of sort " +
                          Domain->getValueAsString("Name") + ", got " +
                          OnSort->getValueAsString("Name"));
  }

  const Record *ToSort = nullptr;
  if (HasTarget) {
    ToSort = coreSortOf(*Root->Kids[1], Records, Rule);
    bool TargetOk =
        llvm::any_of(Targets, [&](const Record *T) { return T == ToSort; });
    if (!TargetOk)
      PrintFatalError(Rule.getLoc(),
                      "core rule: operator '" + Root->Op->getName() +
                          "' does not accept a target of sort " +
                          ToSort->getValueAsString("Name") + "; expected " +
                          coreSortListText(Targets));
  }

  StringRef TargetSort =
      HasTarget ? ToSort->getValueAsString("Name") : StringRef();
  const CoreActionRow *Row = lookupCoreAction(coreTag(*Root->Op), TargetSort);
  if (!Row)
    PrintFatalError(
        Rule.getLoc(),
        "core rule: no action for operator '" + Root->Op->getName() + "'" +
            (HasTarget ? " with target sort " + TargetSort.str() : Twine("")));

  // The guard, when present, is a Bool expression.  Pred and CoreGuard are
  // two spellings of the same slot and cannot both be set.
  const Init *G = Rule.getValueInit("CoreGuard");
  if (G && !isa<UnsetInit>(G)) {
    if (Rule.getValueAsOptionalDef("Pred"))
      PrintFatalError(Rule.getLoc(),
                      "core rule: a rule cannot have both Pred and CoreGuard");
    std::unique_ptr<CoreNode> Guard = parseCoreNode(*G, Rule);
    const Record *GS = coreSortOf(*Guard, Records, Rule);
    if (GS != Records.getDef("SortBool"))
      PrintFatalError(Rule.getLoc(), "core rule: CoreGuard must be Bool, got " +
                                         GS->getValueAsString("Name"));
  }

  CheckedCoreRule C;
  C.Operator = Root->Op;
  C.Action = findCoreAction(Records, Row->Action);
  if (!C.Action)
    PrintFatalError(Rule.getLoc(), "core rule: action table names '" +
                                       Row->Action +
                                       "', which is not a LegalizeAction");
  C.Obs = HasObs ? Root->Kids[0].get() : nullptr;
  C.Target = HasTarget ? Root->Kids[1].get() : nullptr;
  C.Root = std::move(Root);
  // The type_idx projection is checked, not guessed: the only observations a
  // shim can execute are a slot and one element of a slot.  A guard-only
  // terminal has none.
  if (C.Obs)
    C.TypeIdx = coreTypeIdxOf(*C.Obs, Rule);
  return C;
}

/// The observation form a shim can consume.  `ElemOf` is the only observation
/// that changes the mutation payload today; everything else has to be a slot.
static StringRef coreObservationForm(const CoreNode &N) {
  if (N.Kind == CoreNode::IntNode)
    return "slot";
  if (N.Kind == CoreNode::OpNode) {
    if (coreTag(*N.Op) == "slot")
      return "slot";
    if (coreTag(*N.Op) == "elemOf")
      return "elem";
  }
  return "";
}

/// Which table-evaluator translation a shim has.  The C++ shim is always
/// available (the row exists for it); this field is the capability the table
/// backends must check before translating a rule.  A new shim starts at
/// `None`, and the compiler warns when a new value is not handled here.
enum class CoreTableTranslation {
  None,
  WidenVectorMinSize,
  WidenPow2OrMinSize,
  MinScalarOrElt,
  /// The guard alone selects the action; there is no mutation.
  GuardedAction,
  /// `scalarize`: the action is FewerElements and the mutation drops the
  /// lanes, so the evaluator needs both the guard and the vector test.
  Scalarize,
};

struct CoreShimRow {
  StringRef Operator;
  StringRef On;
  StringRef To;
  StringRef Method;
  unsigned Payload;
  CoreTableTranslation Table;
  /// False for a terminal action whose C++ takes only the guard: there is no
  /// type index because there is no mutation.
  bool TakesTypeIdx = true;
  /// The predicate-free method, for a terminal rule with no guard.  `Lower`
  /// and `[CoreAlwaysLower]` have to compile to the same chain call, so the
  /// unguarded form is named here rather than spelled as an always-true
  /// lambda.  Empty when the action has no unconditional method.
  StringRef Unguarded = "";
};

/// One row per primitive the C++ layer can execute.  The checker rejects a
/// rule whose (operator, observation, target) triple has no row, so a schema
/// construct cannot silently degrade into a rule that emits nothing.
static const CoreShimRow CoreShimTable[] = {
    {"widen", "slot", "pow2", "coreWidenScalarToNextPow2", 0,
     CoreTableTranslation::None},
    {"widen", "slot", "pow2Min", "coreWidenScalarToNextPow2", 1,
     CoreTableTranslation::None},
    {"widen", "slot", "multipleOf", "coreWidenScalarToNextMultipleOf", 1,
     CoreTableTranslation::None},
    {"widen", "slot", "ty", "coreMinScalar", 1, CoreTableTranslation::None},
    {"widen", "slot", "sameAs", "coreMinScalarSameAs", 1,
     CoreTableTranslation::None},
    {"widen", "elem", "pow2", "coreWidenScalarOrEltToNextPow2", 0,
     CoreTableTranslation::None},
    {"widen", "elem", "pow2Min", "coreWidenScalarOrEltToNextPow2", 1,
     CoreTableTranslation::None},
    {"widen", "elem", "multipleOf", "coreWidenScalarOrEltToNextMultipleOf", 1,
     CoreTableTranslation::None},
    {"widen", "elem", "ty", "coreMinScalarOrElt", 1,
     CoreTableTranslation::MinScalarOrElt},
    {"widen", "elem", "sameAs", "coreMinScalarSameAs", 1,
     CoreTableTranslation::None},
    {"widen", "elem", "minVectorSize", "coreWidenVectorEltsToVectorMinSize", 1,
     CoreTableTranslation::WidenVectorMinSize},
    {"widen", "slot", "select", "coreWidenScalarOrEltToNextPow2OrMinSize", 1,
     CoreTableTranslation::WidenPow2OrMinSize},
    {"narrow", "slot", "ty", "coreMaxScalar", 1, CoreTableTranslation::None},
    {"narrow", "slot", "sameAs", "coreMaxScalarSameAs", 1,
     CoreTableTranslation::None},
    {"narrow", "elem", "ty", "coreMaxScalarOrElt", 1,
     CoreTableTranslation::None},
    {"more", "slot", "pow2", "coreMoreElementsToNextPow2", 0,
     CoreTableTranslation::None},
    {"fewer", "slot", "ty", "coreFewerElementsToType", 1,
     CoreTableTranslation::None},
    {"bitcast", "slot", "ty", "coreBitcastTo", 1, CoreTableTranslation::None},
    {"legal", "", "", "coreLegalIf", 0, CoreTableTranslation::GuardedAction,
     /*TakesTypeIdx=*/false, /*Unguarded=*/"alwaysLegal"},
    {"custom", "", "", "coreCustomIf", 0, CoreTableTranslation::GuardedAction,
     /*TakesTypeIdx=*/false, /*Unguarded=*/"custom"},
    {"lower", "", "", "coreLowerIf", 0, CoreTableTranslation::GuardedAction,
     /*TakesTypeIdx=*/false, /*Unguarded=*/"lower"},
    {"unsupported", "", "", "coreUnsupportedIf", 0,
     CoreTableTranslation::GuardedAction, /*TakesTypeIdx=*/false,
     /*Unguarded=*/"unsupported"},
    {"scalarize", "slot", "", "coreScalarizeIf", 0,
     CoreTableTranslation::Scalarize, /*TakesTypeIdx=*/true,
     /*Unguarded=*/"scalarize"},
};

static const CoreShimRow *lookupCoreShim(StringRef Operator, StringRef On,
                                         StringRef To) {
  for (const CoreShimRow &Row : CoreShimTable)
    if (Row.Operator == Operator && Row.On == On && Row.To == To)
      return &Row;
  return nullptr;
}

static void coreNodeJson(const CoreNode &N, const RecordKeeper &Records,
                         std::string &Out) {
  if (N.Kind == CoreNode::IntNode) {
    Out = "{\"kind\":\"int\",\"value\":" + std::to_string(N.IntValue) + "}";
    return;
  }
  if (N.Kind == CoreNode::StringNode) {
    Out = "{\"kind\":\"string\",\"value\":" + jsonStringValue(N.StringValue) +
          "}";
    return;
  }
  if (N.Kind == CoreNode::DefNode) {
    const Record *R = N.Op;
    if (R->isSubClassOf("LegalizeNamedType")) {
      Out = "{\"kind\":\"type\",\"value\":" + coreTypeValue(Records, *R) + "}";
      return;
    }
    if (R->isSubClassOf("LegalizeFeature")) {
      Out = "{\"kind\":\"leaf\",\"name\":\"hasFeature\",\"args\":[\"" +
            R->getValueAsString("Cond").str() + "\"]}";
      return;
    }
    if (R->isSubClassOf("LegalizeCoreSet")) {
      const Record *Elem = R->getValueAsDef("ElemSort");
      Out = "{\"kind\":\"set\",\"of\":\"" +
            Elem->getValueAsString("Name").str() + "\",\"elems\":[";
      StringRef Sep;
      for (const Record *E : R->getValueAsListOfDefs("Elems")) {
        Out += Sep.str();
        if (R->isSubClassOf("LegalizeCoreTypeSet"))
          Out += coreTypeValue(Records, *E);
        else
          Out += jsonStringValue(coreTag(*E));
        Sep = ",";
      }
      Out += "]}";
      return;
    }
    Out = "{\"kind\":\"" + coreTag(*R).str() + "\"}";
    return;
  }

  StringRef Name = coreTag(*N.Op);
  if (Name == "slot") {
    Out =
        "{\"kind\":\"slot\",\"index\":" + std::to_string(N.Kids[0]->IntValue) +
        "}";
    return;
  }
  Out = "{\"kind\":\"" + Name.str() + "\"";
  if (!N.Kids.empty()) {
    Out += ",\"args\":[";
    StringRef Sep;
    for (const auto &K : N.Kids) {
      Out += Sep.str();
      std::string Tmp;
      coreNodeJson(*K, Records, Tmp);
      Out += Tmp;
      Sep = ",";
    }
    Out += "]";
  }
  Out += "}";
}

static StringRef coreKindPredicate(StringRef Kind) {
  if (Kind == "kindScalar")
    return "isScalar";
  if (Kind == "kindVector")
    return "isVector";
  if (Kind == "kindScalableVector")
    return "isScalableVector";
  if (Kind == "kindPointer")
    return "isPointer";
  if (Kind == "kindPointerVector")
    return "isPointerVector";
  return "";
}

static StringRef coreOrderingCxx(StringRef Tag) {
  if (Tag == "orderingUnordered")
    return "AtomicOrdering::Unordered";
  if (Tag == "orderingMonotonic")
    return "AtomicOrdering::Monotonic";
  if (Tag == "orderingAcquire")
    return "AtomicOrdering::Acquire";
  if (Tag == "orderingRelease")
    return "AtomicOrdering::Release";
  if (Tag == "orderingAcquireRelease")
    return "AtomicOrdering::AcquireRelease";
  if (Tag == "orderingSequentiallyConsistent")
    return "AtomicOrdering::SequentiallyConsistent";
  return "";
}

/// Sorting values are compared with their predicates rather than with the
/// relational operators, and the two memory orderings share that treatment.
static bool coreIsOrderingObserver(StringRef Tag) {
  return Tag == "memOrdering" || Tag == "memFailureOrdering";
}

/// Emits the C++ expression for one core rule.  Dispatch is a table keyed by
/// the schema tag, so a new observer is one row plus one method, and a tag
/// with no emitter is a diagnostic instead of a silently dropped rule.
class CoreCxxEmitter {
public:
  /// \p QueryVar is the name the surrounding code gives the query object: the
  /// C++ chain wraps a guard in a lambda that calls it `Query`, while the
  /// table evaluator already has `Q` in scope.
  CoreCxxEmitter(const RecordKeeper &Records, const Record &Owner,
                 StringRef QueryVar = "Query")
      : Records(Records), Owner(Owner), QueryVar(QueryVar) {}

  std::string value(const CoreNode &N);
  std::string boolean(const CoreNode &N);

private:
  using EmitFn = std::string (CoreCxxEmitter::*)(const CoreNode &);
  struct Entry {
    StringRef Tag;
    EmitFn Fn;
  };

  const RecordKeeper &Records;
  const Record &Owner;
  StringRef QueryVar;

  [[noreturn]] void fatal(const Twine &Msg) const {
    PrintFatalError(Owner.getLoc(), "core rule: " + Msg);
  }

  std::string childValue(const CoreNode &N, size_t I) {
    if (I >= N.Kids.size())
      fatal("'" + coreTag(*N.Op) + "' is missing operand " +
            std::to_string(I));
    return value(*N.Kids[I]);
  }

  std::string childBool(const CoreNode &N, size_t I) {
    if (I >= N.Kids.size())
      fatal("'" + coreTag(*N.Op) + "' is missing operand " +
            std::to_string(I));
    return boolean(*N.Kids[I]);
  }

  // Value emitters.
  std::string emitSlot(const CoreNode &N) {
    return QueryVar.str() + ".Types[" +
           std::to_string(N.Kids[0]->IntValue) + "]";
  }
  std::string emitMemSlot(const CoreNode &N) {
    return QueryVar.str() + ".MMODescrs[" +
           std::to_string(N.Kids[0]->IntValue) + "]";
  }
  std::string emitElemOf(const CoreNode &N) {
    return "(" + childValue(N, 0) + ").getElementType()";
  }
  std::string emitScalarOf(const CoreNode &N) {
    return "(" + childValue(N, 0) + ").getScalarType()";
  }
  std::string emitNumElems(const CoreNode &N) {
    return "(" + childValue(N, 0) + ").getNumElements()";
  }
  std::string emitBitsOf(const CoreNode &N) {
    return "(" + childValue(N, 0) + ").getScalarSizeInBits()";
  }
  std::string emitSizeOf(const CoreNode &N) {
    // MemDesc::AlignInBits is an alignment, not a size.  SizeOf(memSlot) is
    // the size of the memory type; alignment needs its own observer.
    if (N.Kids[0]->Kind == CoreNode::OpNode &&
        coreTag(*N.Kids[0]->Op) == "memSlot")
      return "(" + childValue(N, 0) + ").MemoryTy.getSizeInBits()";
    return "(" + childValue(N, 0) + ").getSizeInBits()";
  }
  std::string emitAlignOf(const CoreNode &N) {
    return "(" + childValue(N, 0) + ").AlignInBits";
  }
  std::string emitAddrSpace(const CoreNode &N) {
    return "(" + childValue(N, 0) + ").getAddressSpace()";
  }
  std::string emitMemTy(const CoreNode &N) {
    return "(" + childValue(N, 0) + ").MemoryTy";
  }
  std::string emitMemOrdering(const CoreNode &N) {
    return "(" + childValue(N, 0) + ").Ordering";
  }
  std::string emitMemFailureOrdering(const CoreNode &N) {
    return "(" + childValue(N, 0) + ").FailureOrdering";
  }
  std::string emitTy(const CoreNode &N) { return childValue(N, 0); }
  std::string emitSameAs(const CoreNode &N) {
    return QueryVar.str() + ".Types[" +
           std::to_string(N.Kids[0]->IntValue) + "]";
  }
  std::string emitSelect(const CoreNode &N) {
    return "(" + childBool(N, 0) + " ? " + childValue(N, 1) + " : " +
           childValue(N, 2) + ")";
  }

  // Boolean emitters.
  std::string emitTrue(const CoreNode &) { return "true"; }
  std::string emitFeature(const CoreNode &N) {
    return "(" + N.Kids[0]->StringValue + ")";
  }
  std::string junction(const CoreNode &N, StringRef Op) {
    std::string Out;
    for (const auto &K : N.Kids) {
      if (!Out.empty())
        Out += Op;
      Out += "(" + boolean(*K) + ")";
    }
    return Out;
  }
  std::string emitAll(const CoreNode &N) { return junction(N, " && "); }
  std::string emitAny(const CoreNode &N) { return junction(N, " || "); }
  std::string emitNot(const CoreNode &N) {
    return "!(" + childBool(N, 0) + ")";
  }
  std::string emitIsBFloat16(const CoreNode &N) {
    return "(" + childValue(N, 0) + ").isBFloat16()";
  }
  std::string emitIsPow2(const CoreNode &N) {
    return "llvm::isPowerOf2_32(" + childValue(N, 0) + ")";
  }
  std::string emitMul(const CoreNode &N) {
    return "(" + childValue(N, 0) + " * " + childValue(N, 1) + ")";
  }
  std::string emitURem(const CoreNode &N) {
    return "(" + childValue(N, 0) + " % " + childValue(N, 1) + ")";
  }
  std::string emitPow2Ceil(const CoreNode &N) {
    return "llvm::PowerOf2Ceil(" + childValue(N, 0) + ")";
  }
  std::string emitIn(const CoreNode &N);
  std::string emitCompare(const CoreNode &N);
};

std::string CoreCxxEmitter::value(const CoreNode &N) {
  if (N.Kind == CoreNode::IntNode)
    return std::to_string(N.IntValue);
  if (N.Kind == CoreNode::DefNode) {
    if (N.Op->isSubClassOf("LegalizeNamedType"))
      return N.Op->getName().str();
    fatal("'" + N.Op->getName() + "' has no C++ value");
  }
  if (N.Kind != CoreNode::OpNode)
    fatal("operand has no C++ value");

  StringRef Tag = coreTag(*N.Op);
  static const Entry Table[] = {
      {"slot", &CoreCxxEmitter::emitSlot},
      {"memSlot", &CoreCxxEmitter::emitMemSlot},
      {"elemOf", &CoreCxxEmitter::emitElemOf},
      {"scalarOf", &CoreCxxEmitter::emitScalarOf},
      {"numElems", &CoreCxxEmitter::emitNumElems},
      {"pow2Ceil", &CoreCxxEmitter::emitPow2Ceil},
      {"mul", &CoreCxxEmitter::emitMul},
      {"urem", &CoreCxxEmitter::emitURem},
      {"bitsOf", &CoreCxxEmitter::emitBitsOf},
      {"sizeOf", &CoreCxxEmitter::emitSizeOf},
      {"alignOf", &CoreCxxEmitter::emitAlignOf},
      {"addrSpace", &CoreCxxEmitter::emitAddrSpace},
      {"memTy", &CoreCxxEmitter::emitMemTy},
      {"memOrdering", &CoreCxxEmitter::emitMemOrdering},
      {"memFailureOrdering", &CoreCxxEmitter::emitMemFailureOrdering},
      {"ty", &CoreCxxEmitter::emitTy},
      {"sameAs", &CoreCxxEmitter::emitSameAs},
      {"select", &CoreCxxEmitter::emitSelect},
  };
  for (const Entry &E : Table)
    if (E.Tag == Tag)
      return (this->*E.Fn)(N);
  fatal("'" + N.Op->getName() + "' has no C++ value in a guard");
}

std::string CoreCxxEmitter::boolean(const CoreNode &N) {
  if (N.Kind == CoreNode::DefNode && N.Op->isSubClassOf("LegalizeFeature"))
    return N.Op->getName().str();
  if (N.Kind != CoreNode::OpNode)
    fatal("guard is not a Bool node");

  StringRef Tag = coreTag(*N.Op);
  static const Entry Table[] = {
      {"true", &CoreCxxEmitter::emitTrue},
      {"feature", &CoreCxxEmitter::emitFeature},
      {"all", &CoreCxxEmitter::emitAll},
      {"any", &CoreCxxEmitter::emitAny},
      {"not", &CoreCxxEmitter::emitNot},
      {"isPow2", &CoreCxxEmitter::emitIsPow2},
      {"isBFloat16", &CoreCxxEmitter::emitIsBFloat16},
      {"in", &CoreCxxEmitter::emitIn},
      {"eq", &CoreCxxEmitter::emitCompare},
      {"ne", &CoreCxxEmitter::emitCompare},
      {"lt", &CoreCxxEmitter::emitCompare},
      {"le", &CoreCxxEmitter::emitCompare},
      {"gt", &CoreCxxEmitter::emitCompare},
      {"ge", &CoreCxxEmitter::emitCompare},
  };
  for (const Entry &E : Table)
    if (E.Tag == Tag)
      return (this->*E.Fn)(N);
  fatal("'" + N.Op->getName() + "' is not a bool combinator");
}

std::string CoreCxxEmitter::emitIn(const CoreNode &N) {
  const CoreNode &Lhs = *N.Kids[0];
  const CoreNode &Rhs = *N.Kids[1];
  if (Rhs.Kind == CoreNode::OpNode && coreTag(*Rhs.Op) == "clamp") {
    std::string V = value(Lhs);
    return "(" + V + " >= " + std::to_string(Rhs.Kids[0]->IntValue) + " && " +
           V + " <= " + std::to_string(Rhs.Kids[1]->IntValue) + ")";
  }
  if (Rhs.Kind == CoreNode::DefNode &&
      Rhs.Op->isSubClassOf("LegalizeCoreKindSet")) {
    if (Lhs.Kind != CoreNode::OpNode || coreTag(*Lhs.Op) != "sortOf")
      fatal("a Kind set can only test SortOf");
    std::string V = value(*Lhs.Kids[0]);
    std::string Out;
    for (const Record *E : Rhs.Op->getValueAsListOfDefs("Elems")) {
      StringRef Pred = coreKindPredicate(coreTag(*E));
      if (Pred.empty())
        fatal("'" + E->getName() + "' is not a kind");
      if (!Out.empty())
        Out += " || ";
      Out += V + "." + Pred.str() + "()";
    }
    return "(" + Out + ")";
  }
  if (Rhs.Kind == CoreNode::DefNode &&
      Rhs.Op->isSubClassOf("LegalizeCoreTypeSet")) {
    std::string V = value(Lhs);
    std::string Out;
    for (const Record *E : Rhs.Op->getValueAsListOfDefs("Elems")) {
      if (!Out.empty())
        Out += " || ";
      Out += V + " == " + E->getName().str();
    }
    return "(" + Out + ")";
  }
  fatal("unknown set in 'in'");
}

std::string CoreCxxEmitter::emitCompare(const CoreNode &N) {
  StringRef Name = coreTag(*N.Op);
  StringRef Op;
  if (Name == "eq")
    Op = "==";
  else if (Name == "ne")
    Op = "!=";
  else if (Name == "lt")
    Op = "<";
  else if (Name == "le")
    Op = "<=";
  else if (Name == "gt")
    Op = ">";
  else if (Name == "ge")
    Op = ">=";
  else
    fatal("'" + Name + "' is not a bool combinator");

  const CoreNode &Lhs = *N.Kids[0];
  const CoreNode &Rhs = *N.Kids[1];
  // A tag operand may be written bare (`KindVector`) or as a nullary node
  // (`(KindVector)`); both carry the same record, so read the record rather
  // than the node kind.  Requiring OpNode here rejected the bare spelling,
  // which is the one the corpus uses.
  auto TagRecord = [](const CoreNode &N) -> const Record * {
    return N.Kind == CoreNode::OpNode || N.Kind == CoreNode::DefNode ? N.Op
                                                                   : nullptr;
  };
  // SortOf and MemOrdering are tags, not values: compare them with their
  // predicates/atomic-order enumerators instead of spelling a Value.
  if (Lhs.Kind == CoreNode::OpNode && coreTag(*Lhs.Op) == "sortOf") {
    std::string V = value(*Lhs.Kids[0]);
    const Record *Kind = TagRecord(Rhs);
    if (!Kind)
      fatal("SortOf compares against a kind tag");
    StringRef Pred = coreKindPredicate(coreTag(*Kind));
    if (Pred.empty())
      fatal("'" + Kind->getName() + "' is not a kind");
    bool Not = Name == "ne";
    return std::string(Not ? "!(" : "") + V + "." + Pred.str() + "()" +
           (Not ? ")" : "");
  }
  if (Lhs.Kind == CoreNode::OpNode && coreIsOrderingObserver(coreTag(*Lhs.Op))) {
    std::string V = value(Lhs);
    const Record *Ord = TagRecord(Rhs);
    if (!Ord)
      fatal("a memory ordering compares against an ordering tag");
    StringRef O = coreOrderingCxx(coreTag(*Ord));
    if (O.empty())
      fatal("'" + Ord->getName() + "' is not an atomic ordering");
    // AtomicOrdering deletes its relational operators: the lattice is
    // expressed with the named predicates so the generated code says what it
    // means instead of re-deriving the total order.
    if (Name == "eq")
      return "(" + V + " == " + O.str() + ")";
    if (Name == "ne")
      return "(" + V + " != " + O.str() + ")";
    if (Name == "ge")
      return "isAtLeastOrStrongerThan(" + V + ", " + O.str() + ")";
    if (Name == "gt")
      return "isStrongerThan(" + V + ", " + O.str() + ")";
    if (Name == "le")
      return "(!isStrongerThan(" + V + ", " + O.str() + "))";
    if (Name == "lt")
      return "(!isAtLeastOrStrongerThan(" + V + ", " + O.str() + "))";
    fatal("unsupported MemOrdering comparison");
  }
  return "(" + value(Lhs) + " " + Op.str() + " " + value(Rhs) + ")";
}

static void expandCoreRule(const RecordKeeper &Records, const Record &Rule,
                           std::vector<CoreRule> &Out) {
  CheckedCoreRule C = checkCoreRule(Rule, Records);
  CoreRule CR;
  CR.Action = C.Action->getValueAsString("Name").str();
  CR.TypeIdx = C.TypeIdx;
  if (const Record *P = Rule.getValueAsOptionalDef("Pred"))
    predicateJson(Records, *P, CR.Predicate);
  else if (const Init *G = Rule.getValueInit("CoreGuard"))
    if (!isa<UnsetInit>(G)) {
      std::unique_ptr<CoreNode> Guard = parseCoreNode(*G, Rule);
      coreNodeJson(*Guard, Records, CR.Predicate);
    }
  CR.Op = coreTag(*C.Operator).str();
  if (C.Obs)
    coreNodeJson(*C.Obs, Records, CR.On);
  if (C.Target)
    coreNodeJson(*C.Target, Records, CR.To);
  Out.push_back(std::move(CR));
}

/// C2: a table-driven facade must not silently drop an arm.  The action table
/// is checked to be total over every declared operator and every declared
/// target sort before any rule is read, so a new operator is a tablegen error
/// even in a target that never uses it.
static const Record *findCoreOperator(const RecordKeeper &Records,
                                      StringRef Name) {
  for (const Record *Op :
       Records.getAllDerivedDefinitions("LegalizeCoreOperator"))
    if (coreTag(*Op) == Name)
      return Op;
  return nullptr;
}

static const Record *findCoreSort(const RecordKeeper &Records, StringRef Name) {
  for (const Record *S : Records.getAllDerivedDefinitions("LegalizeSort"))
    if (S->getValueAsString("Name") == Name)
      return S;
  return nullptr;
}

static void verifyCoreTables(const RecordKeeper &Records) {
  for (const Record *Op :
       Records.getAllDerivedDefinitions("LegalizeCoreOperator")) {
    if (Op->getValueAsListOfDefs("Targets").empty()) {
      // A terminal operator's action does not depend on a target sort, so the
      // empty sort keys it.  Without this check a new terminal would parse and
      // then fail in every rule that used it.
      if (!lookupCoreAction(coreTag(*Op), ""))
        PrintFatalError(Op->getLoc(),
                        "core action table has no row for terminal operator '" +
                            Op->getName() + "'");
      continue;
    }
    for (const Record *T : Op->getValueAsListOfDefs("Targets"))
      if (!lookupCoreAction(coreTag(*Op), T->getValueAsString("Name")))
        PrintFatalError(Op->getLoc(),
                        "core action table has no row for operator '" +
                            Op->getName() + "' with target sort " +
                            T->getValueAsString("Name"));
  }
  for (const CoreActionRow &Row : CoreActionTable) {
    const Record *Op = findCoreOperator(Records, Row.Operator);
    if (!Op)
      PrintFatalError("core action table names '" + Row.Operator +
                      "', which is not a LegalizeCoreOperator");
    if (Row.TargetSort.empty()) {
      // A terminal row: the empty sort is the key, so it must not name a sort
      // and the operator must not accept one.
      if (!Op->getValueAsListOfDefs("Targets").empty())
        PrintFatalError("core action table gives operator '" + Row.Operator +
                        "' an empty target sort, but the operator has targets");
      continue;
    }
    if (!findCoreSort(Records, Row.TargetSort))
      PrintFatalError("core action table names '" + Row.TargetSort +
                      "', which is not a LegalizeSort");
    if (!findCoreAction(Records, Row.Action))
      PrintFatalError("core action table names action '" + Row.Action +
                      "', which is not a LegalizeAction");
  }
  for (const CoreShimRow &Row : CoreShimTable) {
    if (!findCoreOperator(Records, Row.Operator))
      PrintFatalError("core shim table names '" + Row.Operator +
                      "', which is not a LegalizeCoreOperator");
  }
}

static void verifyCoreRule(const RecordKeeper &Records, const Record &Rule) {
  CheckedCoreRule C = checkCoreRule(Rule, Records);
  StringRef On = C.Obs ? coreObservationForm(*C.Obs) : StringRef();
  StringRef To = C.Target ? coreTag(*C.Target->Op) : StringRef();
  // A terminal rule has no observation to project; only an observation that
  // exists but is not a slot or ElemOf is an error.
  if (C.Obs && On.empty())
    PrintFatalError(Rule.getLoc(),
                    "core rule: no C++ shim for an observation that is neither "
                    "a slot nor ElemOf");
  if (!lookupCoreShim(coreTag(*C.Operator), On, To))
    PrintFatalError(Rule.getLoc(), "core rule: no C++ shim for operator '" +
                                       C.Operator->getName() + "' over '" + On +
                                       "' to '" + To + "'");
}

void LegalizerInfoEmitter::emitCoreRule(const Record &Rule, raw_ostream &OS) {
  CheckedCoreRule C = checkCoreRule(Rule, Records);
  CoreCxxEmitter Cxx(Records, Rule);
  StringRef On = C.Obs ? coreObservationForm(*C.Obs) : StringRef();
  StringRef To = C.Target ? coreTag(*C.Target->Op) : StringRef();
  const CoreShimRow *Shim = lookupCoreShim(coreTag(*C.Operator), On, To);
  if (!Shim)
    PrintFatalError(Rule.getLoc(), "core rule: no C++ shim for operator '" +
                                       C.Operator->getName() + "' over '" + On +
                                       "' to '" + To + "'");

  const Record *Pred = Rule.getValueAsOptionalDef("Pred");
  const Init *G = Rule.getValueInit("CoreGuard");
  const bool HasGuard = Pred || (G && !isa<UnsetInit>(G));
  if (!HasGuard && !Shim->Unguarded.empty()) {
    // A terminal without a guard is the predicate-free method, so a rule
    // written in the vocabulary compiles to the same call as its builder.
    OS << "      ." << Shim->Unguarded << "(";
    if (Shim->TakesTypeIdx)
      OS << C.TypeIdx;
    OS << ")";
    return;
  }

  OS << "      ." << Shim->Method << "(";
  if (Pred) {
    emitPredicate(*Pred, OS);
  } else if (G && !isa<UnsetInit>(G)) {
    std::unique_ptr<CoreNode> Guard = parseCoreNode(*G, Rule);
    OS << "[=](const LegalityQuery &Query) { return " << Cxx.boolean(*Guard)
       << "; }";
  } else {
    OS << "[](const LegalityQuery &) { return true; }";
  }
  if (Shim->TakesTypeIdx)
    OS << ", " << C.TypeIdx;
  if (Shim->Payload) {
    OS << ", ";
    const CoreNode &T = *C.Target;
    if (To == "ty") {
      if (T.Kids[0]->Kind != CoreNode::DefNode ||
          !T.Kids[0]->Op->isSubClassOf("LegalizeNamedType"))
        PrintFatalError(Rule.getLoc(),
                        "core rule: a Ty target must name a LegalizeNamedType");
      OS << T.Kids[0]->Op->getName();
    } else if (To == "select") {
      OS << Cxx.value(T);
    } else if (T.Kids[0]->Kind != CoreNode::IntNode) {
      PrintFatalError(Rule.getLoc(),
                      "core rule: '" + To + "' payload must be a literal int");
    } else {
      OS << T.Kids[0]->IntValue;
    }
  }
  OS << ")";
}

/// One core-rule expansion in flight.
///
/// Each builder handler used to be an `if (Builder == "...")` arm inside
/// expandRule().  The arms are now member functions of this struct and the
/// chain is a table (LoweringHandlers): a builder with no handler is a lookup
/// miss, not an arm that silently falls through the remaining tests.
struct RuleExpander {
  const RecordKeeper &Records;
  const Record &Rule;
  /// The builder's C++ spelling, e.g. "legalFor".  Used in diagnostics.
  StringRef Method;
  /// The builder's TableGen record name, e.g. "BuilderLegalFor".  Handlers
  /// that serve several builders switch on this.
  StringRef Builder;
  ArrayRef<const Record *> Args;
  std::vector<CoreRule> &Out;
  std::string &Why;
  /// The rule's own query predicate, already rendered into Core IR.
  std::string Pred;

  RuleExpander(const RecordKeeper &Records, const Record &Rule,
               ArrayRef<const Record *> Args, std::vector<CoreRule> &Out,
               std::string &Why, std::string PredText)
      : Records(Records), Rule(Rule), Method(builderName(Rule)),
        Builder(builderRecordName(Rule)), Args(Args), Out(Out), Why(Why),
        Pred(std::move(PredText)) {}

  /// The rule's predicate conjoined with a condition the builder adds.  An
  /// empty condition means "true", which is still rendered as a leaf so the
  /// Core IR never has to interpret a missing predicate.
  std::string WithPred(std::string Cond) {
    if (Pred.empty() && Cond.empty())
      return "{\"kind\":\"leaf\",\"name\":\"true\",\"args\":[]}";
    if (Pred.empty())
      return Cond;
    if (Cond.empty())
      return Pred;
    return allOf({Pred, Cond});
  }

  int Int(size_t I) {
    if (I >= Args.size() || !Args[I]->isSubClassOf("LegalizeArgInt"))
      PrintFatalError(Rule.getLoc(), "rule '" + Method +
                                         "' expects an integer at "
                                         "argument " +
                                         std::to_string(I));
    return Args[I]->getValueAsInt("Value");
  }

  std::string TypeValue(size_t I) {
    if (I >= Args.size() || !Args[I]->isSubClassOf("LegalizeNamedType"))
      PrintFatalError(Rule.getLoc(), "rule '" + Method +
                                         "' expects one type at argument " +
                                         std::to_string(I));
    std::string V = coreTypeValue(Records, *Args[I]);
    if (V.empty())
      PrintFatalError(Rule.getLoc(), "rule '" + Method +
                                         "' has a type with no Core IR value "
                                         "at argument " +
                                         std::to_string(I));
    return V;
  }

  std::string TypeBits(size_t I) {
    if (I >= Args.size() || !Args[I]->isSubClassOf("LegalizeNamedType"))
      PrintFatalError(Rule.getLoc(), "rule '" + Method +
                                         "' expects one type at argument " +
                                         std::to_string(I));
    std::string V = coreTypeBitsValue(Records, *Args[I]);
    if (V.empty())
      PrintFatalError(Rule.getLoc(), "rule '" + Method +
                                         "' has a type with no static bit "
                                         "count at argument " +
                                         std::to_string(I));
    return V;
  }

  std::string TypeList(size_t I) {
    if (I >= Args.size())
      PrintFatalError(Rule.getLoc(), "rule '" + Method +
                                         "' is missing argument " +
                                         std::to_string(I));
    std::string V = coreTypeListValue(Records, *Args[I]);
    if (V.empty())
      PrintFatalError(Rule.getLoc(), "rule '" + Method +
                                         "' has a type list with no Core IR "
                                         "value at argument " +
                                         std::to_string(I));
    return V;
  }

  /// The builder has no expansion in this emitter.  The *Expr element counts
  /// and the raw mutation arguments never reach here: the pre-scan in
  /// expandRule() turned them into escapes first.
  [[noreturn]] void noExpansion() {
    PrintFatalError(Rule.getLoc(), "no Core IR expansion for builder '" +
                                       Method +
                                       "'; add it to LegalizerInfo.td first");
  }

  //===--------------------------------------------------------------------===//
  // Builder handlers
  //===--------------------------------------------------------------------===//

  void expandLegalFor() {
    for (size_t AI = 0; AI != Args.size(); ++AI) {
      const Record *A = Args[AI];
      if (A->isSubClassOf("LegalizeTypeTriple")) {
        std::string T0 = coreTypeValue(Records, *A->getValueAsDef("Type0"));
        std::string T1 = coreTypeValue(Records, *A->getValueAsDef("Type1"));
        std::string T2 = coreTypeValue(Records, *A->getValueAsDef("Type2"));
        Out.push_back({"Legal", 0,
                       WithPred(allOf({typeIsValue(0, T0), typeIsValue(1, T1),
                                       typeIsValue(2, T2)})),
                       ""});
      } else if (A->isSubClassOf("LegalizeTypePair")) {
        std::string T0 = coreTypeValue(Records, *A->getValueAsDef("Type0"));
        std::string T1 = coreTypeValue(Records, *A->getValueAsDef("Type1"));
        if (T0.empty() || T1.empty())
          PrintFatalError(A->getLoc(), "legalFor pair has no Core IR value");
        Out.push_back({"Legal", 0,
                       WithPred(allOf({typeIsValue(0, T0), typeIsValue(1, T1)})),
                       ""});
      } else if (A->isSubClassOf("LegalizeNamedType")) {
        Out.push_back(
            {"Legal", 0, WithPred(typeIsValue(0, coreTypeValue(Records, *A))),
             ""});
      } else if (isSelectBearingList(*A)) {
        // A list that chooses between types is one membership test over a
        // conditional list; expanding its branches would duplicate the rule.
        Out.push_back(
            {"Legal", 0, WithPred(typeInValue(0, TypeList(AI))), ""});
      } else {
        // A literal list expands to one rule per element, as it always did:
        // their rule positions are what the witness and mutant artifacts key
        // on, so collapsing them into one typeIn would move every later rule.
        for (const Record *E : A->getValueAsListOfDefs("Elems"))
          Out.push_back({"Legal", 0,
                         WithPred(typeIsValue(0, coreTypeValue(Records, *E))),
                         ""});
      }
    }
  }

  void expandLegalForCartesianProduct() {
    // The C++ builder has one overload per number of type lists (1, 2 and 3);
    // AArch64 uses all three.  Each list is one type slot of the query, so the
    // product is the set of tuples that are legal, and a select-bearing list
    // collapses into a single typeIn predicate on its slot.
    if (Args.empty() || Args.size() > 3)
      PrintFatalError(Rule.getLoc(),
                      "legalForCartesianProduct expects 1 to 3 type lists");
    const unsigned NumLists = Args.size();
    bool AnySelect = false;
    for (unsigned I = 0; I != NumLists; ++I)
      AnySelect |= isSelectBearingList(*Args[I]);
    if (AnySelect) {
      std::vector<std::string> Parts;
      for (unsigned I = 0; I != NumLists; ++I)
        Parts.push_back(typeInValue(I, TypeList(I)));
      Out.push_back({"Legal", 0, WithPred(allOf(Parts)), ""});
      return;
    }
    // Literal lists: one rule per point of the product, leftmost list
    // outermost.  This order is the emitted rule order, so it is observable.
    std::vector<const Record *> Point(NumLists);
    std::function<void(unsigned)> EmitPoint = [&](unsigned I) {
      if (I == NumLists) {
        std::vector<std::string> Parts;
        for (unsigned K = 0; K != NumLists; ++K)
          Parts.push_back(typeIsValue(K, coreTypeValue(Records, *Point[K])));
        Out.push_back({"Legal", 0, WithPred(allOf(Parts)), ""});
        return;
      }
      for (const Record *E : Args[I]->getValueAsListOfDefs("Elems")) {
        Point[I] = E;
        EmitPoint(I + 1);
      }
    };
    EmitPoint(0);
  }

  void expandLegalForTypesWithMemDesc() {
    for (const Record *A : Args) {
      std::string T0 = coreTypeValue(Records, *A->getValueAsDef("Type0"));
      std::string T1 = coreTypeValue(Records, *A->getValueAsDef("Type1"));
      std::string Mem = normalizeLlt(
          A->getValueAsDef("MemTy")->getValueAsString("Expr"), &Records);
      if (T0.empty() || T1.empty())
        PrintFatalError(A->getLoc(), "memdesc value type has no Core IR value");
      // The memory descriptor is its own query axis; the C++ builder passes
      // MMOIdx 0, not type index 2.
      unsigned Align = A->getValueAsInt("Align");
      unsigned PtrLanes = lltPtrLanes(Mem);
      std::string MemPred;
      if (PtrLanes != 0 && Mem.find(":*") != std::string::npos) {
        MemPred = memSizePtr(0, PtrLanes, Align);
      } else {
        StringRef MemElt;
        bool MemScalable = false;
        splitLanes(Mem, MemElt, &MemScalable);
        unsigned MemBits = lltMinBits(Mem);
        if (MemBits == 0)
          PrintFatalError(A->getLoc(),
                          "memdesc memory type '" + Mem + "' has no bit size");
        MemPred = MemScalable ? memSizeScalable(0, MemBits, Align)
                              : memSize(0, MemBits, Align);
      }
      Out.push_back({"Legal", 0,
                     WithPred(
                         allOf({typeIsValue(0, T0), typeIsValue(1, T1), MemPred})),
                     ""});
    }
  }

  void expandAlwaysLegal() { Out.push_back({"Legal", 0, WithPred(""), ""}); }

  // "Unsupported" is not the same observation as "no rule matched": the
  // evaluator reports the first as a rule with action Unsupported and the
  // second as no_match, and diff_core.py counts them apart.  Omitting the
  // builder would have made the explicit form unexpressible.
  void expandUnsupported() {
    Out.push_back({"Unsupported", 0, WithPred(""), ""});
  }

  void expandUnsupportedFor() {
    for (size_t AI = 0; AI != Args.size(); ++AI) {
      const Record *A = Args[AI];
      if (A->isSubClassOf("LegalizeNamedType"))
        Out.push_back({"Unsupported", 0,
                       WithPred(typeIsValue(0, coreTypeValue(Records, *A))), ""});
      else
        Out.push_back({"Unsupported", 0,
                       WithPred(typeInValue(0, TypeList(AI))), ""});
    }
  }

  // customIf(guard): the guard IS the rule, so it becomes the Core IR
  // predicate and the action is the builder's.  Before the predicate AST
  // could hold type leaves this had no literal form, which is why the X86
  // rules that read the query were all escapes.
  void expandCustomIf() {
    if (!Args.empty())
      noExpansion();
    Out.push_back({"Custom", 0, WithPred(""), ""});
  }

  // legalIf(guard) is the "already legal under this guard" form.  It was
  // missing here while customIf and lowerIf were present, so a rule written
  // with it had no Core IR form and the whole rule set escaped -- including
  // the parts of it the vocabulary could describe.
  void expandLegalIf() {
    if (!Args.empty())
      noExpansion();
    Out.push_back({"Legal", 0, WithPred(""), ""});
  }

  void expandLowerIf() {
    if (!Args.empty())
      noExpansion();
    Out.push_back({"Lower", 0, WithPred(""), ""});
  }

  // unsupportedIf(guard): same shape as customIf/legalIf/lowerIf -- the guard
  // is the rule and the builder names the action.  AArch64 uses it for the
  // "this shape is illegal outright" rules, so without it those rule sets had
  // no Core IR form at all.
  void expandUnsupportedIf() {
    if (!Args.empty())
      noExpansion();
    Out.push_back({"Unsupported", 0, WithPred(""), ""});
  }

  void expandLibcallIf() {
    if (!Args.empty())
      noExpansion();
    Out.push_back({"Libcall", 0, WithPred(""), ""});
  }

  void expandLower() { Out.push_back({"Lower", 0, WithPred(""), ""}); }

  void expandLowerFor() {
    for (const Record *A : Args) {
      if (A->isSubClassOf("LegalizeNamedType")) {
        Out.push_back({"Lower", 0,
                       WithPred(typeIsValue(0, coreTypeValue(Records, *A))),
                       ""});
        continue;
      }
      std::string T0 = coreTypeValue(Records, *A->getValueAsDef("Type0"));
      std::string T1 = coreTypeValue(Records, *A->getValueAsDef("Type1"));
      Out.push_back({"Lower", 0,
                     WithPred(allOf({typeIsValue(0, T0), typeIsValue(1, T1)})),
                     ""});
    }
  }

  void expandLibcall() { Out.push_back({"Libcall", 0, WithPred(""), ""}); }

  void expandLibcallForOrCustomFor() {
    StringRef Action = Builder == "BuilderLibcallFor" ? "Libcall" : "Custom";
    for (size_t AI = 0; AI != Args.size(); ++AI) {
      const Record *A = Args[AI];
      if (A->isSubClassOf("LegalizeNamedType"))
        Out.push_back({Action.str(), 0,
                       WithPred(typeIsValue(0, coreTypeValue(Records, *A))), ""});
      else if (A->isSubClassOf("LegalizeTypePair")) {
        std::string T0 = coreTypeValue(Records, *A->getValueAsDef("Type0"));
        std::string T1 = coreTypeValue(Records, *A->getValueAsDef("Type1"));
        Out.push_back(
            {Action.str(), 0,
             WithPred(allOf({typeIsValue(0, T0), typeIsValue(1, T1)})), ""});
      } else
        Out.push_back({Action.str(), 0, WithPred(typeInValue(0, TypeList(AI))),
                       ""});
    }
  }

  void expandCustomForCartesianProduct() {
    std::vector<std::string> Parts = {typeInValue(0, TypeList(0)),
                                      typeInValue(1, TypeList(1))};
    if (Args.size() > 2)
      Parts.push_back(typeInValue(2, TypeList(2)));
    Out.push_back({"Custom", 0, WithPred(allOf(Parts)), ""});
  }

  void expandClampScalar() {
    unsigned Idx = Int(0);
    std::string MinTy = TypeValue(1);
    std::string MaxTy = TypeValue(2);
    Out.push_back({"WidenScalar", Idx,
                   WithPred(scalarNarrowerThanValue(Idx, TypeBits(1))),
                   mutationJsonValue(Idx, "set_elem_type", MinTy)});
    Out.push_back({"NarrowScalar", Idx,
                   WithPred(scalarWiderThanValue(Idx, TypeBits(2))),
                   mutationJsonValue(Idx, "set_elem_type", MaxTy)});
  }

  void expandMinScalar() {
    unsigned Idx = Int(0);
    std::string Ty0 = TypeValue(1);
    Out.push_back({"WidenScalar", Idx,
                   WithPred(scalarNarrowerThanValue(Idx, TypeBits(1))),
                   mutationJsonValue(Idx, "set_elem_type", Ty0)});
  }

  void expandMaxScalar() {
    unsigned Idx = Int(0);
    std::string Ty0 = TypeValue(1);
    Out.push_back({"NarrowScalar", Idx,
                   WithPred(scalarWiderThanValue(Idx, TypeBits(1))),
                   mutationJsonValue(Idx, "set_elem_type", Ty0)});
  }

  // The guarded forms are minScalar/maxScalar with the query predicate
  // conjoined; scalarNarrowerThan/scalarWiderThan already require a scalar,
  // so the C++ lambda's isScalar() check needs no separate leaf here.
  void expandMinScalarIfOrMaxScalarIf() {
    const bool Min = Builder == "BuilderMinScalarIf";
    unsigned Idx = Int(0);
    std::string Ty0 = TypeValue(1);
    Out.push_back({Min ? "WidenScalar" : "NarrowScalar", Idx,
                   WithPred(Min ? scalarNarrowerThanValue(Idx, TypeBits(1))
                                : scalarWiderThanValue(Idx, TypeBits(1))),
                   mutationJsonValue(Idx, "set_elem_type", Ty0)});
  }

  void expandWidenScalarToNextPow2() {
    unsigned Idx = Int(0);
    // The floor is part of the rule: nextPow2OrMin without its Min cannot be
    // evaluated, so the value carries it.
    std::string Target = Args.size() > 1
                             ? "nextPow2OrMin" + std::to_string(Int(1))
                             : std::string("nextPow2");
    // The guard is not "true".  The C++ builder fires only when the size is
    // not a power of two, and the scalar-only builder and the OrElt variant
    // differ on vectors.
    std::string Guard = Builder == "BuilderWidenScalarToNextPow2"
                            ? sizeNotPow2(Idx)
                            : scalarOrEltSizeNotPow2(Idx);
    Out.push_back({"WidenScalar", Idx, WithPred(Guard),
                   mutationJson(Idx, "set_elem_bits", Target)});
  }

  void expandScalarSameSizeAs() {
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
  }

  void expandScalarize() {
    unsigned Idx = Int(0);
    Out.push_back({"FewerElements", Idx, WithPred(isVector(Idx)),
                   mutationJsonInt(Idx, "set_lanes", 1)});
  }

  void expandMoreElementsToNextPow2() {
    unsigned Idx = Int(0);
    Out.push_back({"MoreElements", Idx, WithPred(numElementsNotPow2(Idx)),
                   mutationJson(Idx, "set_lanes", "nextPow2")});
  }

  void expandClampNumElements() {
    unsigned Idx = Int(0);
    // clampNumElements(Idx, MinTy, MaxTy) is the pair
    // clampMinNumElements(Idx, MinTy, lanes(MinTy)).
    //   clampMaxNumElements(Idx, MaxTy, lanes(MaxTy)),
    // so each direction names its own lane count instead of "nextPow2".
    // MinTy names the element type and the lower lane count, so it has to be a
    // literal vector; MaxTy may be a select vector, whose lane count becomes a
    // select rather than one rule per branch.
    std::string MinTy = normalizeLlt(Args[1]->getValueAsString("Expr"), &Records);
    if (MinTy.empty()) {
      Why = "nonliteral_type";
      return;
    }
    std::string MaxLanes = coreTypeLanesValue(Records, *Args[2]);
    if (MaxLanes.empty()) {
      Why = "nonliteral_type";
      return;
    }
    const unsigned MinLanes = lltLanes(MinTy);
    std::string Elem = elementTypeIsValue(Idx, jsonStringValue(lltElt(MinTy)));
    Out.push_back({"MoreElements", Idx,
                   WithPred(allOf({Elem, fewerElementsThanValue(
                                             Idx, std::to_string(MinLanes))})),
                   mutationJsonInt(Idx, "set_lanes", MinLanes)});
    Out.push_back(
        {"FewerElements", Idx,
         WithPred(allOf({Elem, moreElementsThanValue(Idx, MaxLanes)})),
         mutationJsonValue(Idx, "set_lanes", MaxLanes)});
  }

  void expandClampMinOrMaxNumElements() {
    unsigned Idx = Int(0);
    std::string Elt = TypeValue(1);
    std::string Lanes = coreLanesValue(Records, *Args[2]);
    if (Lanes.empty()) {
      Why = "raw_argument";
      return;
    }
    const bool Min = Builder == "BuilderClampMinNumElements";
    Out.push_back(
        {Min ? "MoreElements" : "FewerElements", Idx,
         WithPred(allOf({elementTypeIsValue(Idx, Elt),
                         Min ? fewerElementsThanValue(Idx, Lanes)
                             : moreElementsThanValue(Idx, Lanes)})),
         mutationJsonValue(Idx, "set_lanes", Lanes)});
  }

  void expandCustom() { Out.push_back({"Custom", 0, WithPred(""), ""}); }

  void expandWidenScalarOrEltToNextPow2OrMinSize() {
    unsigned Idx = Int(0);
    std::string Target = Args.size() > 1
                             ? "nextPow2OrMin" + std::to_string(Int(1))
                             : std::string("nextPow2");
    std::string Guard =
        Args.size() > 1
            ? anyOf({scalarOrEltNarrowerThan(Idx, std::to_string(Int(1))),
                     scalarOrEltSizeNotPow2(Idx)})
            : scalarOrEltSizeNotPow2(Idx);
    Out.push_back({"WidenScalar", Idx, WithPred(Guard),
                   mutationJson(Idx, "set_elem_bits", Target)});
  }

  void expandMinScalarOrElt() {
    unsigned Idx = Int(0);
    std::string Ty = TypeValue(1);
    Out.push_back({"WidenScalar", Idx,
                   WithPred(scalarOrEltNarrowerThanValue(Idx, TypeBits(1))),
                   mutationJsonValue(Idx, "set_elem_type", Ty)});
  }

  void expandMinScalarSameAs() {
    unsigned Idx = Int(0);
    unsigned Other = Int(1);
    std::string Ref = "sameAsSlot" + std::to_string(Other);
    Out.push_back({"WidenScalar", Idx,
                   WithPred(scalarSizeLargerThanSlot(Other, Idx)),
                   mutationJson(Idx, "set_elem_bits", Ref)});
  }

  void expandMaxScalarEltSameAsIf() {
    unsigned Idx = Int(0);
    unsigned Other = Int(1);
    std::string Ref = "sameAsSlot" + std::to_string(Other);
    Out.push_back({"NarrowScalar", Idx,
                   WithPred(scalarSizeSmallerThanSlot(Other, Idx)),
                   mutationJson(Idx, "set_elem_bits", Ref)});
  }

  void expandUnsupportedIfMemSizeNotPow2() {
    Out.push_back({"Unsupported", 0, WithPred(memSizeNotPow2(0)), ""});
  }

  void expandLowerIfMemSizeNotPow2() {
    Out.push_back({"Lower", 0, WithPred(memSizeNotPow2(0)), ""});
  }

  void expandLowerIfMemSizeNotByteSizePow2() {
    Out.push_back({"Lower", 0, WithPred(memSizeNotByteSizePow2(0)), ""});
  }

  void expandWidenVectorEltsToVectorMinSize() { Why = "no_core_expansion"; }
};

/// A builder name and the handler that expands rules using it.
struct LoweringHandler {
  StringLiteral Builder;
  void (RuleExpander::*Expand)();
};

/// The dispatch table.  Its keys are LegalizeBuilder record names, so a key
/// that names no declared builder is a tablegen error (verifyLoweringRegistry
/// checks it) instead of a string in an if-chain that can silently rot.
static const LoweringHandler LoweringHandlers[] = {
    {"BuilderLegalFor", &RuleExpander::expandLegalFor},
    {"BuilderLegalForCartesianProduct",
     &RuleExpander::expandLegalForCartesianProduct},
    {"BuilderLegalForTypesWithMemDesc",
     &RuleExpander::expandLegalForTypesWithMemDesc},
    {"BuilderAlwaysLegal", &RuleExpander::expandAlwaysLegal},
    {"BuilderUnsupported", &RuleExpander::expandUnsupported},
    {"BuilderUnsupportedFor", &RuleExpander::expandUnsupportedFor},
    {"BuilderCustomIf", &RuleExpander::expandCustomIf},
    {"BuilderLegalIf", &RuleExpander::expandLegalIf},
    {"BuilderLowerIf", &RuleExpander::expandLowerIf},
    {"BuilderUnsupportedIf", &RuleExpander::expandUnsupportedIf},
    {"BuilderLibcallIf", &RuleExpander::expandLibcallIf},
    {"BuilderLower", &RuleExpander::expandLower},
    {"BuilderLowerFor", &RuleExpander::expandLowerFor},
    {"BuilderLibcall", &RuleExpander::expandLibcall},
    {"BuilderLibcallFor", &RuleExpander::expandLibcallForOrCustomFor},
    {"BuilderCustomFor", &RuleExpander::expandLibcallForOrCustomFor},
    {"BuilderCustomForCartesianProduct",
     &RuleExpander::expandCustomForCartesianProduct},
    {"BuilderClampScalar", &RuleExpander::expandClampScalar},
    {"BuilderClampScalarOrElt", &RuleExpander::expandClampScalar},
    {"BuilderMinScalar", &RuleExpander::expandMinScalar},
    {"BuilderMaxScalar", &RuleExpander::expandMaxScalar},
    {"BuilderMinScalarIf", &RuleExpander::expandMinScalarIfOrMaxScalarIf},
    {"BuilderMaxScalarIf", &RuleExpander::expandMinScalarIfOrMaxScalarIf},
    {"BuilderWidenScalarToNextPow2",
     &RuleExpander::expandWidenScalarToNextPow2},
    {"BuilderWidenScalarOrEltToNextPow2",
     &RuleExpander::expandWidenScalarToNextPow2},
    {"BuilderScalarSameSizeAs", &RuleExpander::expandScalarSameSizeAs},
    {"BuilderScalarize", &RuleExpander::expandScalarize},
    {"BuilderScalarizeIf", &RuleExpander::expandScalarize},
    {"BuilderMoreElementsToNextPow2",
     &RuleExpander::expandMoreElementsToNextPow2},
    {"BuilderClampNumElements", &RuleExpander::expandClampNumElements},
    {"BuilderClampMinNumElements",
     &RuleExpander::expandClampMinOrMaxNumElements},
    {"BuilderClampMaxNumElements",
     &RuleExpander::expandClampMinOrMaxNumElements},
    {"BuilderCustom", &RuleExpander::expandCustom},
    {"BuilderWidenScalarOrEltToNextPow2OrMinSize",
     &RuleExpander::expandWidenScalarOrEltToNextPow2OrMinSize},
    {"BuilderMinScalarOrElt", &RuleExpander::expandMinScalarOrElt},
    {"BuilderMinScalarOrEltIf", &RuleExpander::expandMinScalarOrElt},
    {"BuilderMinScalarSameAs", &RuleExpander::expandMinScalarSameAs},
    {"BuilderMinScalarEltSameAsIf", &RuleExpander::expandMinScalarSameAs},
    {"BuilderMaxScalarEltSameAsIf", &RuleExpander::expandMaxScalarEltSameAsIf},
    {"BuilderUnsupportedIfMemSizeNotPow2",
     &RuleExpander::expandUnsupportedIfMemSizeNotPow2},
    {"BuilderLowerIfMemSizeNotPow2",
     &RuleExpander::expandLowerIfMemSizeNotPow2},
    {"BuilderLowerIfMemSizeNotByteSizePow2",
     &RuleExpander::expandLowerIfMemSizeNotByteSizePow2},
    {"BuilderWidenVectorEltsToVectorMinSize",
     &RuleExpander::expandWidenVectorEltsToVectorMinSize},
};

/// The handler for a builder record, or null when it has no expansion.
static const LoweringHandler *lookupLoweringHandler(StringRef Builder) {
  for (const LoweringHandler &H : LoweringHandlers)
    if (H.Builder == Builder)
      return &H;
  return nullptr;
}

/// Expand one LegalizeRule record into zero or more core rules.
void expandRule(const RecordKeeper &Records, const Record &Rule,
                std::vector<CoreRule> &Out, std::string &Why) {
  Why.clear();
  if (isCoreRule(Rule)) {
    expandCoreRule(Records, Rule, Out);
    return;
  }
  // A rule whose body is raw C++ is a judgment escape.  It produces no core
  // rule; runCore() records it in escapes[] so the count stays visible.
  if (!Rule.getValueAsString("OpaqueArgs").empty()) {
    Why = "opaque_rule";
    return;
  }

  StringRef Builder = builderRecordName(Rule);
  // immIdx() is a coverage marker, not a legalization step: it emits a call
  // that marks an immediate operand as handled and produces no Core IR row.
  if (Builder == "BuilderImmIdx") {
    Why = "coverage_marker";
    return;
  }
  std::vector<const Record *> Args = Rule.getValueAsListOfDefs("Args");

  // Arguments whose value only exists at code-generation time cannot be
  // expanded into a literal Core IR rule.  Naming them here keeps the escape
  // visible instead of guessing a type.
  // A type record whose Expr is a C++ expression has no literal Core IR form.
  // Those rules are escapes, not guesses.
  for (const Record *A : Args) {
    if (A->isSubClassOf("LegalizeTypePair")) {
      if (coreTypeValue(Records, *A->getValueAsDef("Type0")).empty() ||
          coreTypeValue(Records, *A->getValueAsDef("Type1")).empty()) {
        Why = "nonliteral_type";
        return;
      }
      continue;
    }
    if (A->isSubClassOf("LegalizeTypeTriple")) {
      if (coreTypeValue(Records, *A->getValueAsDef("Type0")).empty() ||
          coreTypeValue(Records, *A->getValueAsDef("Type1")).empty() ||
          coreTypeValue(Records, *A->getValueAsDef("Type2")).empty()) {
        Why = "nonliteral_type";
        return;
      }
      continue;
    }
    if (A->isSubClassOf("LegalizeMemDesc")) {
      // The memory-type axis still needs a literal size, so a select memory
      // type stays an escape; the two value types may be select types.
      if (coreTypeValue(Records, *A->getValueAsDef("Type0")).empty() ||
          coreTypeValue(Records, *A->getValueAsDef("Type1")).empty() ||
          normalizeLlt(A->getValueAsDef("MemTy")->getValueAsString("Expr"),
                       &Records)
              .empty()) {
        Why = "nonliteral_type";
        return;
      }
      continue;
    }
    if (A->isSubClassOf("LegalizeSelectTypeList"))
      continue;
    if (A->isSubClassOf("LegalizeNamedType")) {
      if (coreTypeValue(Records, *A).empty()) {
        Why = "nonliteral_type";
        return;
      }
      continue;
    }
    if (A->isSubClassOf("LegalizeNamedTypeList") ||
        A->isSubClassOf("LegalizeInlineTypeList")) {
      for (const Record *E : A->getValueAsListOfDefs("Elems"))
        if (coreTypeValue(Records, *E).empty()) {
          Why = "nonliteral_type";
          return;
        }
      continue;
    }
    if (A->isSubClassOf("LegalizeArgInt") ||
        A->isSubClassOf("LegalizeSelectLanes"))
      continue;
    if (A->isSubClassOf("LegalizeInlineType")) {
      Why = "inline_type_expr";
      return;
    }
    if (A->isSubClassOf("LegalizeRawArg")) {
      Why = "raw_argument";
      return;
    }
    if (A->isSubClassOf("LegalizeRawMutation")) {
      Why = "raw_mutation";
      return;
    }
    if (A->isSubClassOf("LegalizeSelectBits")) {
      Why = "nonliteral_bits";
      return;
    }
  }

  std::string Pred;
  if (const Record *P = Rule.getValueAsOptionalDef("Pred"))
    predicateJson(Records, *P, Pred);

  RuleExpander E(Records, Rule, Args, Out, Why, std::move(Pred));
  const LoweringHandler *H = lookupLoweringHandler(Builder);
  if (!H)
    E.noExpansion();
  (E.*H->Expand)();
}

//===----------------------------------------------------------------------===//
// Lowering registry verification
//===----------------------------------------------------------------------===//

/// Cardinality class of every expansion algorithm.
///
/// This is the C++ half of the `LegalizeExpansion` defs.  The checker requires
/// the two sets to be equal, so adding an algorithm to the .td without teaching
/// the expander about it (or the reverse) is a tablegen error, not a fallback.
struct ExpansionInfo {
  StringLiteral Name;
  /// 0 = always zero rows, 1 = exactly one, 2 = exactly two, 3 = one or more.
  int Cardinality;
};

static constexpr ExpansionInfo Expansions[] = {
    {"ExOne", 1},        {"ExPerType", 3},     {"ExPerPoint", 3},
    {"ExClamp2", 2},     {"ExRawMutation", 0}, {"ExCoverage", 0},
    {"ExSelectBits", 1},
};

static const ExpansionInfo *lookupExpansion(StringRef Name) {
  for (const ExpansionInfo &E : Expansions)
    if (E.Name == Name)
      return &E;
  return nullptr;
}

/// Escape reasons that depend only on an argument record, not on the builder.
/// No entry needs to declare them, because no builder can literalize the
/// argument that produced them.
static bool isGlobalEscapeReason(StringRef Why) {
  return Why == "opaque_rule" || Why == "nonliteral_type" ||
         Why == "inline_type_expr" || Why == "raw_argument";
}

/// Check the lowering registry declared in LegalizerInfo.td.
///
/// Runs once per llvm-tblgen invocation, before anything is emitted, so a
/// builder no target uses is checked too.  Every failure is fatal: the point of
/// the registry is that a missing overload, an undeclared escape and an
/// unhandled algorithm cannot degrade into a fallback.
static LoweringStats verifyLoweringRegistry(const RecordKeeper &Records) {
  LoweringStats S;
  verifyCoreTables(Records);
  std::vector<const Record *> Builders =
      Records.getAllDerivedDefinitions("LegalizeBuilder");
  std::vector<const Record *> Lowerings =
      Records.getAllDerivedDefinitions("LegalizeLowering");
  std::vector<const Record *> ExpansionDefs =
      Records.getAllDerivedDefinitions("LegalizeExpansion");
  std::vector<const Record *> ActionDefs =
      Records.getAllDerivedDefinitions("LegalizeAction");

  S.Builders = Builders.size();
  S.Entries = Lowerings.size();

  // The dispatch table and the builder declarations must agree on spelling: a
  // handler keyed by a name no LegalizeBuilder declares would never run, and
  // two handlers for one builder would shadow each other.
  std::set<StringRef> HandlerKeys;
  for (const LoweringHandler &H : LoweringHandlers)
    if (!HandlerKeys.insert(H.Builder).second)
      PrintFatalError("duplicate lowering handler for builder '" +
                      StringRef(H.Builder) + "'");
  std::set<StringRef> DeclaredBuilders;
  for (const Record *B : Builders)
    DeclaredBuilders.insert(B->getName());
  for (const LoweringHandler &H : LoweringHandlers)
    if (!DeclaredBuilders.count(H.Builder))
      PrintFatalError("lowering handler '" + StringRef(H.Builder) +
                      "' does not name a LegalizeBuilder");

  // Assertion 4 (declaration half): the TD defs and the C++ table agree.
  std::set<std::string> DeclaredExpansions;
  for (const Record *E : ExpansionDefs)
    DeclaredExpansions.insert(E->getValueAsString("Name").str());
  for (const ExpansionInfo &E : Expansions) {
    if (!DeclaredExpansions.erase(E.Name.str()))
      PrintFatalError("legalizer expansion '" + E.Name.str() +
                      "' is handled by the emitter but is not a "
                      "LegalizeExpansion def");
  }
  for (const std::string &Name : DeclaredExpansions)
    PrintFatalError("legalizer expansion '" + Name +
                    "' has no emitter handling");

  // Assertion 1: one entry per (builder, overload), and no extras.
  std::map<std::pair<const Record *, int>, const Record *> Entries;
  for (const Record *L : Lowerings) {
    const Record *B = L->getValueAsDef("Builder");
    if (!B->isSubClassOf("LegalizeBuilder"))
      PrintFatalError(L->getLoc(), "lowering '" + L->getName() +
                                       "' does not name a builder");
    int Overload = (int)L->getValueAsInt("Overload");
    if (!Entries.emplace(std::make_pair(B, Overload), L).second)
      PrintFatalError(L->getLoc(), "duplicate lowering for builder '" +
                                       B->getName() + "' overload " +
                                       std::to_string(Overload));
  }
  for (const Record *B : Builders) {
    unsigned N = B->getValueAsListInit("Formals")->size();
    S.Overloads += N;
    for (unsigned O = 0; O != N; ++O)
      if (!Entries.count(std::make_pair(B, (int)O)))
        PrintFatalError(B->getLoc(),
                        "builder '" + B->getName() + "' declares overload " +
                            std::to_string(O) + " but has no lowering for it");
  }
  if (Entries.size() != S.Overloads)
    PrintFatalError("lowering registry has " + std::to_string(Entries.size()) +
                    " entries for " + std::to_string(S.Overloads) +
                    " declared overloads");

  // Assertions 2, 3: actions and coverage.  A shim is not a property of a
  // (builder, overload) entry: the same builder reaches different primitives
  // depending on its target, so the shim table is keyed by
  // (operator, observation, target) and verified by verifyCoreTables().
  //
  // The actions a Core IR row can carry.  Composite actions (clampScalar and
  // friends) are directions rather than rows, and Coverage never reaches the
  // Core IR: it marks an immediate operand as handled.
  std::map<std::string, std::vector<const Record *>> BuildersByAction;
  for (const Record *B : Builders)
    BuildersByAction[B->getValueAsDef("Action")->getValueAsString("Name").str()]
        .push_back(B);

  std::set<std::string> CoveredActions;
  for (const Record *L : Lowerings) {
    StringRef ExpName = L->getValueAsDef("Expansion")->getValueAsString("Name");
    const ExpansionInfo *Info = lookupExpansion(ExpName);
    if (!Info)
      PrintFatalError(L->getLoc(), "unknown expansion '" + ExpName + "'");
    std::vector<const Record *> Actions = L->getValueAsListOfDefs("Actions");
    std::vector<StringRef> Escapes = L->getValueAsListOfStrings("Escapes");
    if (Info->Cardinality == 0) {
      if (!Actions.empty())
        PrintFatalError(L->getLoc(), "zero-row expansion '" + ExpName +
                                         "' declares actions");
      if (Escapes.empty())
        PrintFatalError(L->getLoc(), "zero-row expansion '" + ExpName +
                                         "' declares no escape reason");
    } else if (Actions.empty()) {
      PrintFatalError(L->getLoc(),
                      "expansion '" + ExpName + "' declares no action");
    }
    for (const Record *A : Actions) {
      if (!A->isSubClassOf("LegalizeAction"))
        PrintFatalError(L->getLoc(),
                        "'" + A->getName() + "' is not a LegalizeAction");
      CoveredActions.insert(A->getValueAsString("Name").str());
    }
    if (!L->getValueAsBit("TableTranslation"))
      ++S.EntriesWithoutTable;
  }
  // Assertion 3: every action a rule could emit is named by an entry.  An
  // action with no emitting entry is allowed only when every builder that
  // declares it has a zero-row expansion -- today that is Bitcast, whose only
  // builder carries a raw mutation.
  for (const Record *A : ActionDefs) {
    std::string Name = A->getValueAsString("Name").str();
    if (Name == "Coverage" || Name == "ClampScalar" ||
        Name == "ScalarSameSizeAs" || Name == "ClampNumElements")
      continue;
    if (CoveredActions.count(Name))
      continue;
    bool Reachable = false;
    for (const Record *B : BuildersByAction[Name]) {
      auto It = Entries.find(std::make_pair(B, 0));
      if (It == Entries.end())
        continue;
      StringRef ExpName =
          It->second->getValueAsDef("Expansion")->getValueAsString("Name");
      if (lookupExpansion(ExpName)->Cardinality != 0)
        Reachable = true;
    }
    if (Reachable)
      PrintFatalError("action '" + Name +
                      "' is reachable but no lowering emits it");
  }

  // Assertions 2 and 5 (per rule): the registry must describe what the
  // existing expander actually produces, and a zero-row result must name a
  // reason its entry declares.
  std::vector<const Record *> RuleSets =
      Records.getAllDerivedDefinitions("LegalizeRuleSet");
  S.RuleSets = RuleSets.size();
  for (const Record *RS : RuleSets) {
    bool WithoutTable = false;
    for (const Record *Rule : RS->getValueAsListOfDefs("Rules")) {
      ++S.Rules;
      if (isCoreRule(*Rule)) {
        ++S.CoreRules;
        verifyCoreRule(Records, *Rule);
        continue;
      }
      const Record *B = Rule->getValueAsDef("Builder");
      int Overload = matchedOverloadIndex(*Rule);
      if (Overload < 0) {
        // This check runs before verifyRule(), so a rule that no overload
        // accepts would otherwise stop here with a message that names neither
        // the guard kind nor the declared signatures.  Report it the way
        // verifyRule() would, then fall back if it somehow passes.
        verifyGuardKind(*Rule, builderName(*Rule), B);
        verifyArgumentShape(*Rule, builderName(*Rule), B);
        PrintFatalError(Rule->getLoc(),
                        "rule uses builder '" + B->getName() +
                            "' with arguments no declared overload accepts");
      }
      auto It = Entries.find(std::make_pair(B, Overload));
      if (It == Entries.end())
        PrintFatalError(Rule->getLoc(), "no lowering for builder '" +
                                            B->getName() + "' overload " +
                                            std::to_string(Overload));
      const Record *L = It->second;
      if (!L->getValueAsBit("TableTranslation"))
        WithoutTable = true;

      // immIdx is a coverage marker: runCore emits neither a row nor an escape
      // for it, so the registry check must not count one either.
      if (B->getName() == "BuilderImmIdx")
        continue;

      StringRef ExpName =
          L->getValueAsDef("Expansion")->getValueAsString("Name");
      int Cardinality = lookupExpansion(ExpName)->Cardinality;

      std::vector<CoreRule> Out;
      std::string Why;
      expandRule(Records, *Rule, Out, Why);

      if (!Why.empty()) {
        ++S.Escapes;
        std::vector<StringRef> Escapes = L->getValueAsListOfStrings("Escapes");
        if (!isGlobalEscapeReason(Why) && !llvm::is_contained(Escapes, Why))
          PrintFatalError(Rule->getLoc(), "rule escapes with '" + Why +
                                              "' but " + B->getName() +
                                              " overload " +
                                              std::to_string(Overload) +
                                              " does not declare it");
        continue;
      }
      if (Cardinality == 0)
        PrintFatalError(Rule->getLoc(),
                        "zero-row expansion '" + ExpName + "' produced rows");
      if ((Cardinality == 1 || Cardinality == 2) &&
          Out.size() != (size_t)Cardinality)
        PrintFatalError(Rule->getLoc(), "expansion '" + ExpName + "' expects " +
                                            std::to_string(Cardinality) +
                                            " row(s), got " +
                                            std::to_string(Out.size()));
      if (Out.empty())
        PrintFatalError(Rule->getLoc(),
                        "rule produced no rows and reported no escape");
      std::set<std::string> Allowed;
      for (const Record *A : L->getValueAsListOfDefs("Actions"))
        Allowed.insert(A->getValueAsString("Name").str());
      for (const CoreRule &R : Out)
        if (!Allowed.count(R.Action))
          PrintFatalError(Rule->getLoc(), "rule emits action '" + R.Action +
                                              "' but " + B->getName() +
                                              " overload " +
                                              std::to_string(Overload) +
                                              " does not declare it");
    }
    if (WithoutTable)
      ++S.RuleSetsWithoutTable;
  }
  return S;
}

} // namespace

void LegalizerInfoEmitter::runCore(raw_ostream &OS) {
  verifyLoweringRegistry(Records);

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
    std::string Norm = normalizeLlt(T->getValueAsString("Expr"), &Records);
    if (!Norm.empty() && Norm.find(":*") == std::string::npos)
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
    // The owner is the target this file describes, taken from its name, so
    // one emitter serves every target .td instead of naming X86 in an
    // AArch64 report.
    StringRef Owner = originFile(Rule);
    Owner = Owner.rsplit('/').second;
    Owner.consume_front("/");
    if (Owner.ends_with("LegalizerInfo.td"))
      Owner = Owner.drop_back(strlen("LegalizerInfo.td"));
    S += ", \"owner\": ";
    S += jsonEscaped(Owner);
    S += ", \"removal\": \"express it with the Core IR "
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
  // The file version is the highest schema any entry requires, so a target
  // that has not adopted the primitive vocabulary stays 0.3 and keeps its
  // frozen artifacts byte-identical.  Entries that do need 0.4 carry their
  // own `schema` tag; the file version is what tells a reader whether it can
  // skip the file, the tag is what tells it which entries it may lower.
  bool HasCoreRule = llvm::any_of(RuleSets, [](const Record *RS) {
    return llvm::any_of(RS->getValueAsListOfDefs("Rules"), [](const Record *R) {
      return isCoreRule(*R);
    });
  });
  OS << "{\n";
  OS << "  \"schema\": \"legalizer-spec-core\",\n";
  OS << "  \"version\": \"" << (HasCoreRule ? "0.4" : "0.3") << "\",\n";
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
    std::vector<const Record *> Rules = RuleSet->getValueAsListOfDefs("Rules");
    for (size_t RuleIdx = 0; RuleIdx != Rules.size(); ++RuleIdx) {
      const Record *Rule = Rules[RuleIdx];
      if (!isCoreRule(*Rule) &&
          builderRecordName(*Rule) == "BuilderImmIdx")
        continue;
      std::vector<CoreRule> Expanded;
      std::string Why;
      expandRule(Records, *Rule, Expanded, Why);
      if (!Why.empty()) {
        if (Why == "opaque_rule")
          AddEscape("judgment", "opaque_rule",
                    "rule body is raw C++ (" + builderName(*Rule).str() + ")",
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
        if (!CR.Op.empty()) {
          // A primitive entry is readable only by a 0.4 reader.  Tag it so a
          // reader can tell it apart from a legacy entry without inferring
          // the version from the presence of `op`; entries without the tag
          // are 0.3, which is why a 0.3-only target stays byte-identical.
          S += ", \"schema\": \"0.4\"";
          S += ", \"op\": ";
          S += jsonStringValue(CR.Op);
          // A terminal operator has neither an observation nor a target, and
          // writing an empty node for either would invent one.
          if (!CR.On.empty())
            S += ", \"on\": " + CR.On;
          if (!CR.To.empty())
            S += ", \"to\": " + CR.To;
        }
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

cl::opt<std::string>
    TablePrefix("legalizer-table-prefix",
                cl::desc("identifier prefix for the generated legalizer table"),
                cl::init("X86"));

/// A target whose rule chains are only defined on the shapes the rules
/// themselves name (AArch64's element-count lambdas call
/// LLT::getNumElements(), which is undefined for a scalar or a scalable
/// vector) cannot be probed with synthetic shapes.  The domain then stays the
/// tuples the rules name, which are well-formed by construction.
cl::opt<bool> TableTuplesOnly(
    "legalizer-table-tuples-only",
    cl::desc("emit only the query shapes the rules name, without the pointer "
             "probe and the residual shapes"),
    cl::init(false));

enum TableTypeKind {
  TablePointer,
  TableScalar,
  TableInteger,
  TableFloat,
  TableBFloat,
  TableVector,
  TablePtrVector,
  TableScalableVector,
  TableScalablePtrVector
};

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
  /// The classes of the memory-descriptor axis.  Unlike a type slot, a
  /// descriptor is compared with the chain's TypePairAndMemDesc, which keeps
  /// the memory type's identity, so a vector of pointers is its own class
  /// here rather than an "outside" answer.
  std::vector<TableType> MemClasses;

  bool resolve(StringRef Expr, StringRef Root, SmallVectorImpl<TableType> &Out,
               unsigned Depth) {
    Expr = stripParens(Expr);
    if (Depth > 16)
      return false;

    StringRef Then, Else;
    if (splitConditional(Expr, Then, Else))
      return resolve(Then, Root, Out, Depth) && resolve(Else, Root, Out, Depth);

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
    if (ArgOf("LLT::integer(", Arg)) {
      unsigned Bits;
      if (Arg.trim().getAsInteger(10, Bits))
        return false;
      Out.push_back({TableInteger, Bits, 1, Root.str()});
      return true;
    }
    if (Expr == "LLT::float16()") {
      Out.push_back({TableFloat, 16, 1, Root.str()});
      return true;
    }
    if (Expr == "LLT::float32()") {
      Out.push_back({TableFloat, 32, 1, Root.str()});
      return true;
    }
    if (Expr == "LLT::float64()") {
      Out.push_back({TableFloat, 64, 1, Root.str()});
      return true;
    }
    if (Expr == "LLT::float128()") {
      Out.push_back({TableFloat, 128, 1, Root.str()});
      return true;
    }
    if (Expr == "LLT::bfloat16()") {
      Out.push_back({TableBFloat, 16, 1, Root.str()});
      return true;
    }
    if (ArgOf("LLT::scalable_vector(", Arg)) {
      size_t Comma = Arg.find(',');
      if (Comma == StringRef::npos)
        return false;
      unsigned Lanes;
      if (Arg.substr(0, Comma).trim().getAsInteger(10, Lanes))
        return false;
      StringRef Elem = Arg.substr(Comma + 1).trim();
      unsigned ElemBits;
      if (!Elem.getAsInteger(10, ElemBits)) {
        Out.push_back({TableScalableVector, ElemBits, Lanes, Root.str()});
        return true;
      }
      SmallVector<TableType, 2> Elems;
      if (!resolve(Elem, Root, Elems, Depth + 1))
        return false;
      for (const TableType &E : Elems) {
        switch (E.Kind) {
        case TableScalar:
        case TableInteger:
        case TableFloat:
        case TableBFloat:
          Out.push_back({TableScalableVector, E.ElemBits, Lanes, Root.str()});
          break;
        case TablePointer:
          Out.push_back({TableScalablePtrVector, 0, Lanes, Root.str()});
          break;
        default:
          return false;
        }
      }
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
        switch (E.Kind) {
        case TableScalar:
        case TableInteger:
        case TableFloat:
        case TableBFloat:
          Out.push_back({TableVector, E.ElemBits, Lanes, Root.str()});
          break;
        case TablePointer:
          // A vector of pointers is a different LLT from a vector of integers
          // of the same shape; the class list cannot express it yet.
          Out.push_back({TablePtrVector, 0, Lanes, Root.str()});
          break;
        default:
          return false;
        }
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
      // A select type names a condition, not a class: the classes it can
      // produce are its branches, which are named types of their own and are
      // collected when they are reached.
      if (T->isSubClassOf("LegalizeSelectType"))
        continue;
      SmallVector<TableType, 4> Resolved;
      SmallVector<TableType, 4> Out;
      StringRef Expr = T->getValueAsString("Expr");
      if (!resolve(Expr, T->getName(), Resolved, 0))
        PrintFatalError(T->getLoc(),
                        "cannot derive a table type class from '" + Expr + "'");
      for (const TableType &R : Resolved) {
        // A vector of pointers is a distinct LLT from an integer vector of
        // the same shape, and the class kind keeps them apart, so it belongs
        // in the type axis too.
        if (!Seen.insert({R.Kind, R.ElemBits, R.Lanes}).second)
          continue;
        Out.push_back(R);
      }
      Classes.insert(Classes.end(), Out.begin(), Out.end());
    }

    llvm::sort(Classes, lessTypeClass);
    if (Classes.empty() || Classes.front().Kind != TablePointer)
      PrintFatalError("a legalizer table needs a pointer type class");
  }

  static int typeRank(TableTypeKind K) {
    switch (K) {
    case TablePointer:
      return 0;
    case TableScalar:
      return 1;
    case TableInteger:
      return 2;
    case TableFloat:
      return 3;
    case TableBFloat:
      return 4;
    case TableVector:
      return 5;
    case TablePtrVector:
      return 6;
    case TableScalableVector:
      return 7;
    case TableScalablePtrVector:
      return 8;
    }
    llvm_unreachable("bad table type kind");
  }

  static bool lessTypeClass(const TableType &A, const TableType &B) {
    if (typeRank(A.Kind) != typeRank(B.Kind))
      return typeRank(A.Kind) < typeRank(B.Kind);
    if (A.ElemBits != B.ElemBits)
      return A.ElemBits < B.ElemBits;
    return A.Lanes < B.Lanes;
  }

  /// Collect the classes of the memory-descriptor axis.
  ///
  /// A descriptor carries an LLT, and the rules key on its identity: the
  /// memdesc guards compare the memory type by size, but G_STORE's guard says
  /// "the value type is the memory type", which is an identity comparison.  So
  /// this axis uses full LLT classes, including vectors of pointers.  The
  /// classes come from the same named types as the type axis, so a memory type
  /// the .td never names is outside the table and falls back to the chain.
  void collectMemClasses() {
    std::vector<const Record *> Types =
        Records.getAllDerivedDefinitions("LegalizeNamedType");
    llvm::sort(Types, LessRecordByID());

    std::set<std::tuple<int, unsigned, unsigned>> Seen;
    for (const Record *T : Types) {
      if (T->isSubClassOf("LegalizeSelectType"))
        continue;
      SmallVector<TableType, 4> Resolved;
      if (!resolve(T->getValueAsString("Expr"), T->getName(), Resolved, 0))
        PrintFatalError(T->getLoc(),
                        "cannot derive a memory type class from '" +
                            T->getValueAsString("Expr") + "'");
      for (const TableType &R : Resolved)
        if (Seen.insert({R.Kind, R.ElemBits, R.Lanes}).second)
          MemClasses.push_back(R);
    }
    // The pointer class is the one width the .td leaves to the subtarget, and
    // it is also the self-check probe for a memdesc rule set.
    if (Seen.insert({TablePointer, 0, 1}).second)
      MemClasses.push_back({TablePointer, 0, 1, "p0"});
    llvm::sort(MemClasses, lessTypeClass);
    // Keep the generated arrays well-formed when nothing names a descriptor.
    if (MemClasses.empty())
      MemClasses.push_back(Classes.front());
  }

  static StringRef tableTypeKindName(TableTypeKind K) {
    switch (K) {
    case TablePointer:
      return "Kind_Pointer";
    case TableScalar:
      return "Kind_Scalar";
    case TableInteger:
      return "Kind_Integer";
    case TableFloat:
      return "Kind_Float";
    case TableBFloat:
      return "Kind_BFloat";
    case TableVector:
      return "Kind_Vector";
    case TablePtrVector:
      return "Kind_PtrVector";
    case TableScalableVector:
      return "Kind_ScalableVector";
    case TableScalablePtrVector:
      return "Kind_ScalablePtrVector";
    }
    llvm_unreachable("bad table type kind");
  }

  /// The same spelling for the test-only metadata, where the reader's enum is
  /// not visible.
  static std::string testTypeKindName(TableTypeKind K, StringRef Prefix) {
    StringRef Suffix;
    switch (K) {
    case TablePointer:
      Suffix = "TestKind_Pointer";
      break;
    case TableScalar:
      Suffix = "TestKind_Scalar";
      break;
    case TableInteger:
      Suffix = "TestKind_Integer";
      break;
    case TableFloat:
      Suffix = "TestKind_Float";
      break;
    case TableBFloat:
      Suffix = "TestKind_BFloat";
      break;
    case TableVector:
      Suffix = "TestKind_Vector";
      break;
    case TablePtrVector:
      Suffix = "TestKind_PtrVector";
      break;
    case TableScalableVector:
      Suffix = "TestKind_ScalableVector";
      break;
    case TableScalablePtrVector:
      Suffix = "TestKind_ScalablePtrVector";
      break;
    default:
      llvm_unreachable("bad table type kind");
    }
    return (Prefix + Suffix).str();
  }

  /// True when a predicate record reads the memory descriptor, directly or as
  /// a dag leaf.  Such a rule set needs the descriptor axis in its key.
  static bool predicateReadsMemDesc(const Record *P) {
    if (!P)
      return false;
    if (P->isSubClassOf("LegalizeQueryPredicate"))
      return P->getValueAsString("Expr").contains("MMODescrs");
    if (P->isSubClassOf("LegalizeTypePredicate"))
      return P->getValueAsString("Call").contains("MMODescrs");
    bool Found = false;
    if (P->isSubClassOf("LegalizeQueryPredicateDag") ||
        P->isSubClassOf("LegalizeBoolPredicateDag")) {
      std::function<void(const Init *)> Walk = [&](const Init *I) {
        if (const auto *D = dyn_cast<DagInit>(I)) {
          for (const Init *Arg : D->getArgs())
            Walk(Arg);
          return;
        }
        if (const auto *Def = dyn_cast<DefInit>(I))
          if (Def->getDef() != P)
            Found |= predicateReadsMemDesc(Def->getDef());
      };
      Walk(P->getValueAsDag("CondDag"));
    }
    return Found;
  }

  /// Apply \p Pred to a core rule's observation and guard, stopping at the
  /// first node it accepts.  The metadata walks below read the C++ text a
  /// legacy predicate carries; a core rule has a tree instead, and both of its
  /// dag fields are part of what the rule reads.  A visitor that only collects
  /// information returns false throughout.
  static bool anyCoreNode(const Record &Rule,
                          const std::function<bool(const CoreNode &)> &Pred) {
    for (StringRef Field : {"CoreExpr", "CoreGuard"}) {
      const Init *I = Rule.getValueInit(Field);
      if (!I || isa<UnsetInit>(I))
        continue;
      std::unique_ptr<CoreNode> N = parseCoreNode(*I, Rule);
      if (Pred(*N))
        return true;
    }
    return false;
  }

  /// Slots a core rule reads.  The observation names one too (`Scalarize`'s
  /// slot is the only record of what it rewrites), and `SameAs(k)` indexes
  /// `Types[k]` without spelling a `Slot`.
  static void scanCoreSlots(const CoreNode &N, unsigned &Arity) {
    if (N.Kind != CoreNode::OpNode)
      return;
    StringRef Tag = coreTag(*N.Op);
    if ((Tag == "slot" || Tag == "sameAs") && !N.Kids.empty() &&
        N.Kids[0]->Kind == CoreNode::IntNode)
      Arity = std::max(Arity, unsigned(N.Kids[0]->IntValue + 1));
    for (const auto &K : N.Kids)
      scanCoreSlots(*K, Arity);
  }

  static void scanCoreRuleSlots(const Record &Rule, unsigned &Arity) {
    anyCoreNode(Rule, [&](const CoreNode &N) {
      scanCoreSlots(N, Arity);
      return false;
    });
  }

  /// True when a core rule reads the memory descriptor.  `(MemTy (MemSlot 0))`
  /// is a guard, but the walk covers the observation as well so that a future
  /// memdesc-observing operator does not silently drop the descriptor axis.
  static bool coreReadsMemDesc(const CoreNode &N) {
    if (N.Kind != CoreNode::OpNode)
      return false;
    if (coreTag(*N.Op) == "memSlot")
      return true;
    for (const auto &K : N.Kids)
      if (coreReadsMemDesc(*K))
        return true;
    return false;
  }

  /// True when the rule set's key needs the memory descriptor: either a rule
  /// names one, or a guard reads one.
  static bool needsMemDesc(const Record &RuleSet) {
    for (const Record *Rule : RuleSet.getValueAsListOfDefs("Rules")) {
      for (const Record *Arg : Rule->getValueAsListOfDefs("Args"))
        if (Arg->isSubClassOf("LegalizeMemDesc"))
          return true;
      if (Rule->getValueAsString("OpaqueArgs").contains("MMODescrs"))
        return true;
      if (predicateReadsMemDesc(Rule->getValueAsOptionalDef("Pred")))
        return true;
      if (isCoreRule(*Rule) && anyCoreNode(*Rule, coreReadsMemDesc))
        return true;
    }
    return false;
  }

  /// True when a rule reads something the key does not carry.  The memory
  /// descriptor is part of the key (see needsMemDesc), so a guard may read
  /// MMODescrs[0]; a type slot past 1 or the opcode is still an escape.
  static bool hasOpaqueEscape(const Record &Rule) {
    if (!Rule.isSubClassOf("LegalizeOpaqueRule"))
      return false;
    StringRef Body = Rule.getValueAsString("OpaqueArgs");
    return Body.contains("Query.Opcode") || Body.contains("Opcode ==") ||
           Body.contains("Types[2]") || Body.contains("Types[3]") ||
           Body.contains("MMODescrs[1]");
  }

  static bool tabulated(const Record &RuleSet) {
    for (const Record *Rule : RuleSet.getValueAsListOfDefs("Rules"))
      if (hasOpaqueEscape(*Rule))
        return false;
    return true;
  }

  /// The alignment thresholds the tabulated memdesc rules name.  A query's
  /// alignment is classified by how many of them it meets; that is enough
  /// because TypePairAndMemDesc::isCompatible only asks Align >= Entry.Align.
  std::vector<unsigned> alignThresholds() {
    std::set<unsigned> Thresholds;
    for (const Record *RS :
         Records.getAllDerivedDefinitions("LegalizeRuleSet")) {
      if (!tabulated(*RS) || !needsMemDesc(*RS))
        continue;
      for (const Record *Rule : RS->getValueAsListOfDefs("Rules"))
        for (const Record *Arg : Rule->getValueAsListOfDefs("Args"))
          if (Arg->isSubClassOf("LegalizeMemDesc"))
            Thresholds.insert(Arg->getValueAsInt("Align"));
    }
    return std::vector<unsigned>(Thresholds.begin(), Thresholds.end());
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
    case TableInteger:
      return "LLT::integer(" + std::to_string(T.ElemBits) + ")";
    case TableFloat:
      switch (T.ElemBits) {
      case 16:
        return "LLT::float16()";
      case 32:
        return "LLT::float32()";
      case 64:
        return "LLT::float64()";
      case 128:
        return "LLT::float128()";
      }
      llvm_unreachable("bad float width");
    case TableBFloat:
      return "LLT::bfloat16()";
    case TableVector:
      return "LLT::fixed_vector(" + std::to_string(T.Lanes) + ", " +
             std::to_string(T.ElemBits) + ")";
    case TablePtrVector:
      return "LLT::fixed_vector(" + std::to_string(T.Lanes) +
             ", LLT::pointer(0, PointerBits))";
    case TableScalableVector:
      return "LLT::scalable_vector(" + std::to_string(T.Lanes) + ", " +
             std::to_string(T.ElemBits) + ")";
    case TableScalablePtrVector:
      return "LLT::scalable_vector(" + std::to_string(T.Lanes) +
             ", LLT::pointer(0, PointerBits))";
    }
    llvm_unreachable("bad table type kind");
  }

  /// One query shape a rule names, with the memory descriptor it names (if
  /// any).  The descriptor is a separate axis of the key, not a type slot: the
  /// builder takes it as an MMO index.
  struct RuleTuple {
    std::vector<TableType> Types;
    bool HasMem = false;
    TableType MemTy{TableScalar, 0, 0, ""};
    unsigned Align = 0;
  };

  /// Type tuples a rule names as a complete query shape.  A builder that takes
  /// two separate type arguments (ClampScalar's bounds) names bounds, not a
  /// query, so only list/pair/memdesc arguments are used.
  void collectRuleTuples(const Record &Rule, std::vector<RuleTuple> &Out) {
    if (isCoreRule(Rule))
      return;
    std::vector<const Record *> Args = Rule.getValueAsListOfDefs("Args");
    // A builder that takes a slot index treats its type arguments as bounds
    // (ClampScalar<0, s8, sMaxScalar>), not as a query shape.  Without an
    // index, a list of types is a legalFor-style group and each type is one
    // unary query.
    bool HasIndex = false;
    for (const Record *Arg : Args)
      if (Arg->isSubClassOf("LegalizeArgIndex"))
        HasIndex = true;

    std::function<void(const Record &, std::vector<TableType> &)> ResolveOne =
        [&](const Record &R, std::vector<TableType> &Tys) {
      // A select type can yield any of its branches depending on the
      // configuration, so the domain has to contain all of them.
      if (R.isSubClassOf("LegalizeSelectType")) {
        for (const Record *B : R.getValueAsListOfDefs("Types"))
          ResolveOne(*B, Tys);
        ResolveOne(*R.getValueAsDef("Otherwise"), Tys);
        return;
      }
      StringRef Expr = R.isSubClassOf("LegalizeInlineType")
                           ? R.getValueAsString("Text")
                           : R.getValueAsString("Expr");
      SmallVector<TableType, 2> Resolved;
      if (!resolve(Expr, R.getName(), Resolved, 0))
        PrintFatalError(R.getLoc(),
                        "cannot resolve '" + Expr + "' for the table domain");
      Tys.insert(Tys.end(), Resolved.begin(), Resolved.end());
    };

    // Expand a list-shaped argument: a named/inline list, or a choice between
    // two named lists.
    std::function<void(const Record &, std::vector<TableType> &)> ExpandList =
        [&](const Record &List, std::vector<TableType> &L) {
          if (List.isSubClassOf("LegalizeSelectTypeList")) {
            for (const Record *B : List.getValueAsListOfDefs("Lists"))
              ExpandList(*B, L);
            ExpandList(*List.getValueAsDef("Otherwise"), L);
            return;
          }
          for (const Record *E : List.getValueAsListOfDefs("Elems")) {
            std::vector<TableType> Tys;
            ResolveOne(*E, Tys);
            L.insert(L.end(), Tys.begin(), Tys.end());
          }
        };

    // legalForCartesianProduct({a, b}, {c}) is a pair query, not two unary
    // ones: each argument list contributes one slot.  The same holds for
    // customForCartesianProduct, which AArch64 uses for G_VAARG.
    if (builderName(Rule).ends_with("ForCartesianProduct")) {
      std::vector<std::vector<TableType>> Lists;
      for (const Record *Arg : Args) {
        if (!Arg->isSubClassOf("LegalizeNamedTypeList") &&
            !Arg->isSubClassOf("LegalizeInlineTypeList") &&
            !Arg->isSubClassOf("LegalizeSelectTypeList"))
          continue;
        std::vector<TableType> L;
        ExpandList(*Arg, L);
        if (!L.empty())
          Lists.push_back(std::move(L));
      }
      std::vector<TableType> Cur;
      std::function<void(size_t)> Product = [&](size_t I) {
        if (I == Lists.size()) {
          Out.push_back({Cur});
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
          Arg->isSubClassOf("LegalizeSelectTypeList")) {
        std::vector<TableType> L;
        ExpandList(*Arg, L);
        for (const TableType &T : L)
          Out.push_back({{T}});
      } else if (Arg->isSubClassOf("LegalizeTypePair")) {
        std::vector<TableType> Ty;
        ResolveOne(*Arg->getValueAsDef("Type0"), Ty);
        ResolveOne(*Arg->getValueAsDef("Type1"), Ty);
        Out.push_back({Ty});
      } else if (Arg->isSubClassOf("LegalizeMemDesc")) {
        // The memory type is part of MMODescr, not of Types; the query shape
        // the reader sees is (value, pointer) plus the descriptor.
        RuleTuple T;
        ResolveOne(*Arg->getValueAsDef("Type0"), T.Types);
        ResolveOne(*Arg->getValueAsDef("Type1"), T.Types);
        std::vector<TableType> Mem;
        const Record *MemTy = Arg->getValueAsDef("MemTy");
        ResolveOne(*MemTy, Mem);
        if (Mem.size() != 1)
          PrintFatalError(Arg->getLoc(),
                          "a memory type has to name exactly one class");
        T.HasMem = true;
        T.MemTy = Mem.front();
        T.Align = Arg->getValueAsInt("Align");
        Out.push_back(T);
      } else if (!HasIndex && (Arg->isSubClassOf("LegalizeNamedType") ||
                               Arg->isSubClassOf("LegalizeInlineType"))) {
        std::vector<TableType> Ty;
        ResolveOne(*Arg, Ty);
        for (const TableType &T : Ty)
          Out.push_back({{T}});
      }
    }
  }

  /// ---------------------------------------------------------------------
  /// Evaluators
  ///
  /// The table does not "run the chain and cache the answer": the code below
  /// is the chain itself, with each guard and mutation written out against the
  /// same prologue -gen-legalizer emits.  A rule set with a rule this cannot
  /// translate is opaque, and the reader falls back to the chain for it.
  /// ---------------------------------------------------------------------

  /// C++ expression for one type argument.  An inline type is parenthesized so
  /// it can stand as an argument or before a method call.
  static std::string typeText(const Record &Arg) {
    if (Arg.isSubClassOf("LegalizeNamedType"))
      return Arg.getName().str();
    if (Arg.isSubClassOf("LegalizeInlineType"))
      return "(" + squashSpace(Arg.getValueAsString("Text")) + ")";
    PrintFatalError(Arg.getLoc(), "the table evaluator cannot name the type '" +
                                      Arg.getName() + "'");
  }

  /// Elements of a named or inline type list, as C++ expressions.
  static std::vector<std::string> listElements(const Record &List) {
    std::vector<std::string> Out;
    for (const Record *E : List.getValueAsListOfDefs("Elems"))
      Out.push_back(typeText(*E));
    return Out;
  }

  /// C++ expression usable as the range of a range-for over a type list: the
  /// identifier a named list was declared with, a conditional between two of
  /// them, or a braced list for an inline one.
  static std::string typeListText(const Record &Arg) {
    if (Arg.isSubClassOf("LegalizeNamedTypeList"))
      return Arg.getName().str();
    if (Arg.isSubClassOf("LegalizeSelectTypeList"))
      return selectText(Arg, "Lists", "Otherwise");
    if (Arg.isSubClassOf("LegalizeInlineTypeList")) {
      std::string S = "{";
      StringRef Sep;
      for (const std::string &E : listElements(Arg)) {
        S += Sep.str();
        S += E;
        Sep = ", ";
      }
      return S + "}";
    }
    PrintFatalError(Arg.getLoc(),
                    "the table evaluator cannot name the type list '" +
                        Arg.getName() + "'");
  }

  /// A lane count or bit width chosen by a priority list of cached booleans.
  static std::string selectIntText(const Record &Arg) {
    std::vector<const Record *> Conds = Arg.getValueAsListOfDefs("Conds");
    std::vector<int64_t> Values = Arg.getValueAsListOfInts("Values");
    if (Conds.size() != Values.size())
      PrintFatalError(Arg.getLoc(),
                      "'" + Arg.getName() + "' has " +
                          std::to_string(Conds.size()) + " guards but " +
                          std::to_string(Values.size()) + " values");
    std::string Expr = std::to_string(Arg.getValueAsInt("Otherwise"));
    for (size_t I = Conds.size(); I-- > 0;) {
      std::string Cond;
      raw_string_ostream C(Cond);
      renderPredicate(*Conds[I], C);
      C.flush();
      Expr =
          "(" + Cond + " ? " + std::to_string(Values[I]) + " : " + Expr + ")";
    }
    return Expr;
  }

  /// A lane count argument: a literal, a structured select, or the C++ text the
  /// .td carried.
  static std::string laneText(const Record &Arg) {
    if (Arg.isSubClassOf("LegalizeArgLanes"))
      return std::to_string(Arg.getValueAsInt("Value"));
    if (Arg.isSubClassOf("LegalizeSelectLanes"))
      return selectIntText(Arg);
    if (Arg.isSubClassOf("LegalizeRawArg"))
      return "(" + squashSpace(Arg.getValueAsString("Text")) + ")";
    PrintFatalError(Arg.getLoc(),
                    "the table evaluator cannot read a lane count from '" +
                        Arg.getName() + "'");
  }

  /// A bit width argument: a literal or a structured select.
  static std::string bitsText(const Record &Arg) {
    if (Arg.isSubClassOf("LegalizeArgInt"))
      return std::to_string(Arg.getValueAsInt("Value"));
    if (Arg.isSubClassOf("LegalizeSelectBits"))
      return selectIntText(Arg);
    PrintFatalError(Arg.getLoc(),
                    "the table evaluator cannot read a bit width from '" +
                        Arg.getName() + "'");
  }

  /// One type-slot test.  The same predicate the chain stores, called on the
  /// query.
  static std::string typeIsAt(StringRef Idx, const Record &Ty) {
    return "typeIs(" + Idx.str() + ", " + typeText(Ty) + ")(Q)";
  }

  /// The rule's guard as a complete C++ boolean expression.  A cached bool is
  /// used as it stands; a query predicate renders as a callable, so it is
  /// called with the query.
  static std::string guardText(const Record &Rule) {
    const Record *P = Rule.getValueAsOptionalDef("Pred");
    if (!P)
      return "";
    std::string S;
    raw_string_ostream OS(S);
    renderPredicate(*P, OS);
    OS.flush();
    if (P->isSubClassOf("LegalizeQueryPredicateBase"))
      return "(" + S + ")(Q)";
    return S;
  }

  static std::string conjunction(StringRef A, StringRef B) {
    if (A.empty())
      return B.str();
    if (B.empty())
      return A.str();
    return ("(" + A + ") && (" + B + ")").str();
  }

  /// One returned step.  Without a mutation the step is the action alone; with
  /// one, the mutation is called on the query exactly as the chain would.
  ///
  /// With RuleIdx set the step is stored in Out and the rule that produced it
  /// is named by its 0-based position in the rule set's Rules list, then the
  /// evaluator reports a match instead of returning the step.  That is the
  /// test-only rule-index form (-gen-legalizer-rule-index); the numbering is
  /// the one X86LegalizerInfo.core.json records as rule_index.
  static void emitStep(raw_ostream &OS, StringRef Action, StringRef Indent,
                       StringRef Mutation, StringRef RuleIdx = "",
                       StringRef MutationsFlag = "") {
    if (RuleIdx.empty()) {
      if (Mutation.empty()) {
        OS << Indent << "return {LegalizeActions::" << Action
           << ", 0, LLT{}};\n";
        return;
      }
      OS << Indent << "auto M = (" << Mutation << ")(Q);\n";
      OS << Indent << "return {LegalizeActions::" << Action
         << ", M.first, M.second};\n";
      return;
    }
    if (Mutation.empty()) {
      OS << Indent << "Out = {LegalizeActions::" << Action << ", 0, LLT{}};\n";
    } else if (!MutationsFlag.empty()) {
      // The mutation is what makes the step concrete, but a domain point the
      // real chain would never see (a pointer in a scalar slot) can assert in
      // LLT.  A caller that only needs the rule identity clears the flag.
      OS << Indent << "if (" << MutationsFlag << ") {\n";
      OS << Indent << "  auto M = (" << Mutation << ")(Q);\n";
      OS << Indent << "  Out = {LegalizeActions::" << Action
         << ", M.first, M.second};\n";
      OS << Indent << "} else {\n";
      OS << Indent << "  Out = {LegalizeActions::" << Action
         << ", 0, LLT{}};\n";
      OS << Indent << "}\n";
    } else {
      OS << Indent << "auto M = (" << Mutation << ")(Q);\n";
      OS << Indent << "Out = {LegalizeActions::" << Action
         << ", M.first, M.second};\n";
    }
    OS << Indent << "RuleIndex = " << RuleIdx << ";\n";
    OS << Indent << "return true;\n";
  }

  /// One returned step whose new type is spelled out rather than computed by a
  /// mutation factory: the clamp builders produce their type inline.
  static void emitTypedStep(raw_ostream &OS, StringRef Action, StringRef Indent,
                            StringRef TypeIdx, StringRef NewType,
                            StringRef RuleIdx = "") {
    if (RuleIdx.empty()) {
      OS << Indent << "return {LegalizeActions::" << Action << ", " << TypeIdx
         << ", " << NewType << "};\n";
      return;
    }
    OS << Indent << "Out = {LegalizeActions::" << Action << ", " << TypeIdx
       << ", " << NewType << "};\n";
    OS << Indent << "RuleIndex = " << RuleIdx << ";\n";
    OS << Indent << "return true;\n";
  }

  /// One rule being translated into evaluator statements.
  ///
  /// The arms of translateRule() are member functions of this struct and the
  /// dispatch is RuleTranslators.  Everything the arms shared while they were
  /// inline -- the rule, its arguments, the output buffer, the guard and the
  /// Fail()/Finish() pair -- is a member here, so each body reads as before.
  struct RuleTranslation {
    const Record &Rule;
    StringRef Name;
    StringRef Action;
    std::vector<const Record *> Args;
    std::string Guard;
    const unsigned *RuleIdx;
    bool GuardOnly;
    std::string RuleIdxText;
    std::string MutationsFlag;
    std::string Buf;
    raw_string_ostream OS;
    std::string &Out;
    std::string &Why;

    RuleTranslation(const Record &Rule, StringRef Prefix, std::string &Out,
                    std::string &Why, const unsigned *RuleIdx, bool GuardOnly)
        : Rule(Rule), Name(builderName(Rule)),
          Action(Rule.getValueAsDef("Builder")
                     ->getValueAsDef("Action")
                     ->getValueAsString("Name")),
          Args(Rule.getValueAsListOfDefs("Args")), Guard(guardText(Rule)),
          RuleIdx(RuleIdx), GuardOnly(GuardOnly),
          RuleIdxText(RuleIdx ? std::to_string(*RuleIdx) : ""),
          MutationsFlag(RuleIdx ? (Prefix + "EvalRuleIndexMutations").str()
                                : std::string()),
          OS(Buf), Out(Out), Why(Why) {}

    bool Fail() {
      Why = "'" + Name.str() + "' has no table translation";
      return false;
    }

    std::string IdxOf(size_t I) {
      return std::to_string(Args[I]->getValueAsInt("Value"));
    }

    void NeedArgs(size_t N) {
      if (Args.size() != N)
        PrintFatalError(Rule.getLoc(), "legalizer builder '" + Name + "' has " +
                                           std::to_string(Args.size()) +
                                           " arguments; the table evaluator "
                                           "expected " +
                                           std::to_string(N));
    }

    void EmitIf(StringRef Cond, StringRef Act, StringRef Mut = "") {
      OS << "  if (" << Cond << ") {\n";
      if (GuardOnly)
        OS << "    return true;\n";
      else
        emitStep(OS, Act, "    ", Mut, RuleIdxText, MutationsFlag);
      OS << "  }\n";
    }

    void EmitTypedIf(StringRef Cond, StringRef Act, StringRef Idx,
                     StringRef Ty) {
      OS << "  if (" << Cond << ") {\n";
      if (GuardOnly)
        OS << "    return true;\n";
      else
        emitTypedStep(OS, Act, "    ", Idx, Ty, RuleIdxText);
      OS << "  }\n";
    }

    bool Finish() {
      OS.flush();
      Out = std::move(Buf);
      return true;
    }

    /// An opaque body is a query predicate, and the rule builder takes it as a
    /// guard.  The table evaluator has the query, so it can call the body the
    /// same way the chain does: `if ((<body>)(Q))`.  Only guard-only builders
    /// qualify; one whose action needs a mutation would lose it here.
    bool translateOpaqueBody() {
      const Record *B = Rule.getValueAsDef("Builder");
      if (!B->getValueAsDef("Action")->getValueAsString("Mutation").empty()) {
        Why = "'" + Name.str() + "' carries an opaque body and a mutation";
        return false;
      }
      std::string Text;
      raw_string_ostream BOS(Text);
      emitOpaqueBody(BOS, Rule.getValueAsString("OpaqueArgs"));
      BOS.flush();
      OS << "  if ((" << Text << ")(Q)) {\n";
      if (GuardOnly)
        OS << "    return true;\n";
      else
        emitStep(OS, Action, "    ", "", RuleIdxText);
      OS << "  }\n";
      return Finish();
    }

    // An action with no guard, no arguments and no mutation.
    bool translateBareAction() {
      if (!Args.empty() || !Guard.empty())
        return Fail();
      if (GuardOnly)
        OS << "  return true;\n";
      else
        emitStep(OS, Action, "  ", "", RuleIdxText);
      return Finish();
    }

    // An action whose only guard is a query predicate (legalIf, lowerIf, ...).
    bool translateGuardedAction() {
      if (!Args.empty() || Guard.empty())
        return Fail();
      EmitIf(Guard, Action);
      return Finish();
    }

    // legalFor / lowerFor / libcallFor / customFor: one type in slot 0, or a
    // list of pairs, depending on the arguments.
    bool translateActionFor() {
      if (Args.empty())
        return Fail();
      const Record *Front = Args.front();
      const bool Pairs = Front->isSubClassOf("LegalizeTypePair");
      const bool Triples = Front->isSubClassOf("LegalizeTypeTriple");
      std::string Cond;
      StringRef Sep;
      for (const Record *A : Args) {
        Cond += Sep.str();
        if (Pairs) {
          if (!A->isSubClassOf("LegalizeTypePair"))
            return Fail();
          Cond += "(" + typeIsAt("0", *A->getValueAsDef("Type0")) + " && " +
                  typeIsAt("1", *A->getValueAsDef("Type1")) + ")";
        } else if (Triples) {
          // legalFor with triples tests all three slots, exactly like the
          // expander's one-row-per-triple form.
          if (!A->isSubClassOf("LegalizeTypeTriple"))
            return Fail();
          Cond += "(" + typeIsAt("0", *A->getValueAsDef("Type0")) + " && " +
                  typeIsAt("1", *A->getValueAsDef("Type1")) + " && " +
                  typeIsAt("2", *A->getValueAsDef("Type2")) + ")";
        } else if (A->isSubClassOf("LegalizeNamedTypeList") ||
                   A->isSubClassOf("LegalizeInlineTypeList")) {
          std::string Or;
          StringRef Sep2;
          for (const std::string &E : listElements(*A)) {
            Or += Sep2.str();
            Or += "typeIs(0, " + E + ")(Q)";
            Sep2 = " || ";
          }
          Cond += "(" + Or + ")";
        } else {
          Cond += "typeIs(0, " + typeText(*A) + ")(Q)";
        }
        Sep = " || ";
      }
      EmitIf(conjunction(Guard, Cond), Action);
      return Finish();
    }

    // The cartesian-product builders: one nested loop per list, so the answer
    // is per query type rather than per class pair.  The number of lists is the
    // builder's overload (1 to 3); a one-list rule used to abort the backend
    // because the translation demanded exactly two.
    bool translateCartesian() {
      if (Args.empty() || Args.size() > 3)
        return Fail();
      const size_t N = Args.size();
      std::string Cond;
      StringRef Sep;
      for (size_t I = 0; I != N; ++I) {
        Cond += Sep.str();
        Cond +=
            "typeIs(" + std::to_string(I) + ", T" + std::to_string(I) + ")(Q)";
        Sep = " && ";
      }
      for (size_t I = 0; I != N; ++I)
        OS << std::string(2 * I + 2, ' ') << "for (const LLT T" << I << " : "
           << typeListText(*Args[I]) << ")\n";
      const std::string Indent(2 * N + 2, ' ');
      OS << Indent << "if (" << conjunction(Guard, Cond) << ") {\n";
      if (GuardOnly)
        OS << Indent << "  return true;\n";
      else
        emitStep(OS, Action, Indent + "  ", "", RuleIdxText, MutationsFlag);
      OS << Indent << "}\n";
      return Finish();
    }

    // legalForTypesWithMemDesc: the chain's guard is exactly the factory call
    // this rule set would have made, and the evaluator receives the query, so
    // the descriptor is compared rather than approximated by the key.
    bool translateMemDesc() {
      if (Args.empty() || !all_of(Args, [](const Record *A) {
            return A->isSubClassOf("LegalizeMemDesc");
          }))
        return Fail();
      std::string List;
      StringRef Sep;
      for (const Record *A : Args) {
        List += Sep.str();
        List += "{" + typeText(*A->getValueAsDef("Type0")) + ", " +
                typeText(*A->getValueAsDef("Type1")) + ", " +
                typeText(*A->getValueAsDef("MemTy")) + ", " +
                std::to_string(A->getValueAsInt("Align")) + "}";
        Sep = ", ";
      }
      EmitIf(conjunction(Guard,
                         "typePairAndMemDescInSet(0, 1, 0, {" + List + "})(Q)"),
             Action);
      return Finish();
    }

    // clampScalar expands to minScalar + maxScalar; clampScalarOrElt to the
    // scalar-or-element pair.  Both directions share one mutation.
    bool translateClampScalar() {
      NeedArgs(3);
      const bool OrElt = Name == "clampScalarOrElt";
      const std::string Idx = IdxOf(0);
      const std::string Min = typeText(*Args[1]);
      const std::string Max = typeText(*Args[2]);
      const StringRef Size =
          OrElt ? "getScalarSizeInBits()" : "getSizeInBits()";
      EmitIf(conjunction(Guard, std::string(OrElt ? "scalarOrEltNarrowerThan"
                                                  : "scalarNarrowerThan") +
                                    "(" + Idx + ", (" + Min + ")." +
                                    Size.str() + ")(Q)"),
             "WidenScalar",
             "LegalizeMutations::changeElementSizeTo(" + Idx + ", " + Min +
                 ")");
      EmitIf(conjunction(Guard, std::string(OrElt ? "scalarOrEltWiderThan"
                                                  : "scalarWiderThan") +
                                    "(" + Idx + ", (" + Max + ")." +
                                    Size.str() + ")(Q)"),
             "NarrowScalar",
             "LegalizeMutations::changeElementSizeTo(" + Idx + ", " + Max +
                 ")");
      return Finish();
    }

    // minScalarIf / maxScalarIf: the builder's lambda adds the scalar and
    // the direction check on top of the query guard.
    bool translateScalarIf() {
      NeedArgs(2);
      const bool Min = Name == "minScalarIf";
      const std::string Idx = IdxOf(0);
      const std::string Ty = typeText(*Args[1]);
      const std::string V = "Q.Types[" + Idx + "]";
      EmitIf(conjunction(Guard, V + ".isScalar() && " + V +
                                    ".getSizeInBits() " + (Min ? "< " : "> ") +
                                    "(" + Ty + ").getSizeInBits()"),
             Min ? "WidenScalar" : "NarrowScalar",
             "LegalizeMutations::changeElementSizeTo(" + Idx + ", " + Ty + ")");
      return Finish();
    }

    bool translateMinMaxScalar() {
      NeedArgs(2);
      const bool Min = Name == "minScalar";
      const std::string Idx = IdxOf(0);
      const std::string Ty = typeText(*Args[1]);
      EmitIf(conjunction(
                 Guard,
                 std::string(Min ? "scalarNarrowerThan" : "scalarWiderThan") +
                     "(" + Idx + ", (" + Ty + ").getSizeInBits())(Q)"),
             Min ? "WidenScalar" : "NarrowScalar",
             "LegalizeMutations::changeElementSizeTo(" + Idx + ", " + Ty + ")");
      return Finish();
    }

    // scalarSameSizeAs expands to minScalarSameAs + maxScalarSameAs.
    bool translateScalarSameSizeAs() {
      NeedArgs(2);
      const std::string Idx = IdxOf(0);
      const std::string Other = IdxOf(1);
      const std::string Mut =
          "LegalizeMutations::changeElementSizeTo(" + Idx + ", " + Other + ")";
      const std::string This = "Q.Types[" + Idx + "].getScalarSizeInBits()";
      const std::string That = "Q.Types[" + Other + "].getScalarSizeInBits()";
      EmitIf(conjunction(Guard, That + " > " + This), "WidenScalar", Mut);
      EmitIf(conjunction(Guard, That + " < " + This), "NarrowScalar", Mut);
      return Finish();
    }

    bool translateScalarize() {
      NeedArgs(1);
      const std::string Idx = IdxOf(0);
      EmitIf(conjunction(Guard, "isVector(" + Idx + ")(Q)"), "FewerElements",
             "LegalizeMutations::scalarize(" + Idx + ")");
      return Finish();
    }

    bool translateMoreElementsToNextPow2() {
      NeedArgs(1);
      const std::string Idx = IdxOf(0);
      EmitIf(conjunction(Guard, "numElementsNotPow2(" + Idx + ")(Q)"),
             "MoreElements",
             "LegalizeMutations::moreElementsToNextPow2(" + Idx + ")");
      return Finish();
    }

    bool translateWidenScalarToNextPow2() {
      if (Args.empty() || Args.size() > 2)
        return Fail();
      const std::string Idx = IdxOf(0);
      const std::string Min =
          Args.size() > 1 ? std::to_string(Args[1]->getValueAsInt("Value"))
                          : "0";
      EmitIf(conjunction(Guard, std::string(Name == "widenScalarToNextPow2"
                                                ? "sizeNotPow2"
                                                : "scalarOrEltSizeNotPow2") +
                                    "(" + Idx + ")(Q)"),
             "WidenScalar",
             "LegalizeMutations::widenScalarOrEltToNextPow2(" + Idx + ", " +
                 Min + ")");
      return Finish();
    }

    // clampNumElements expands to clampMinNumElements + clampMaxNumElements.
    bool translateClampNumElements() {
      NeedArgs(3);
      const std::string Idx = IdxOf(0);
      const std::string Min = typeText(*Args[1]);
      const std::string Max = typeText(*Args[2]);
      const std::string V = "Q.Types[" + Idx + "]";
      EmitTypedIf(conjunction(Guard, V + ".isFixedVector() && " + V +
                                         ".getElementType() == (" + Min +
                                         ").getElementType() && " + V +
                                         ".getNumElements() < (" + Min +
                                         ").getNumElements()"),
                  "MoreElements", Idx,
                  "LLT::fixed_vector((" + Min + ").getNumElements(), " + V +
                      ".getElementType())");
      EmitTypedIf(conjunction(Guard, V + ".isFixedVector() && " + V +
                                         ".getElementType() == (" + Max +
                                         ").getElementType() && " + V +
                                         ".getNumElements() > (" + Max +
                                         ").getNumElements()"),
                  "FewerElements", Idx,
                  "LLT::scalarOrVector(ElementCount::getFixed((" + Max +
                      ").getNumElements()), " + V + ".getElementType())");
      return Finish();
    }

    bool translateClampMinMaxNumElements() {
      NeedArgs(3);
      const bool MinSide = Name == "clampMinNumElements";
      const std::string Idx = IdxOf(0);
      const std::string Elt = typeText(*Args[1]);
      const std::string Lanes = laneText(*Args[2]);
      const std::string V = "Q.Types[" + Idx + "]";
      EmitTypedIf(conjunction(Guard, V + ".isFixedVector() && " + V +
                                         ".getElementType() == " + Elt +
                                         " && " + V + ".getNumElements() " +
                                         (MinSide ? "< " : "> ") + Lanes),
                  MinSide ? "MoreElements" : "FewerElements", Idx,
                  MinSide ? "LLT::fixed_vector(" + Lanes + ", " + V +
                                ".getElementType())"
                          : "LLT::scalarOrVector(ElementCount::getFixed(" +
                                Lanes + "), " + V + ".getElementType())");
      return Finish();
    }
    /// unsupportedFor(Types): one rule per type, like the C++ builder's
    /// actionFor.
    bool translateUnsupportedFor() {
      if (Args.empty())
        return Fail();
      std::string Cond;
      StringRef Sep;
      for (const Record *A : Args) {
        Cond += Sep.str();
        if (A->isSubClassOf("LegalizeNamedType")) {
          Cond += "typeIs(0, " + typeText(*A) + ")(Q)";
        } else if (A->isSubClassOf("LegalizeNamedTypeList") ||
                   A->isSubClassOf("LegalizeInlineTypeList")) {
          std::string Or;
          StringRef Sep2;
          for (const std::string &E : listElements(*A)) {
            Or += Sep2.str();
            Or += "typeIs(0, " + E + ")(Q)";
            Sep2 = " || ";
          }
          Cond += "(" + Or + ")";
        } else {
          return Fail();
        }
        Sep = " || ";
      }
      EmitIf(conjunction(Guard, Cond), Action);
      return Finish();
    }

    // minScalarOrElt / minScalarOrEltIf: the guard is the scalar-or-element
    // narrowness test, the mutation names the type.
    bool translateMinScalarOrElt() {
      NeedArgs(2);
      const std::string Idx = IdxOf(0);
      const std::string Ty = typeText(*Args[1]);
      EmitIf(conjunction(Guard, "scalarOrEltNarrowerThan(" + Idx + ", (" + Ty +
                                    ").getScalarSizeInBits())(Q)"),
             "WidenScalar",
             "LegalizeMutations::changeElementSizeTo(" + Idx + ", " + Ty + ")");
      return Finish();
    }

    // minScalarSameAs / minScalarEltSameAsIf: widen to the other slot's
    // scalar size.  changeElementSizeTo(Idx, Other) is exactly the lambda the
    // builder writes.
    bool translateMinScalarSameAs() {
      NeedArgs(2);
      const std::string Idx = IdxOf(0);
      const std::string Other = IdxOf(1);
      EmitIf(conjunction(Guard, "Q.Types[" + Other +
                                    "].getScalarSizeInBits() > Q.Types[" + Idx +
                                    "].getScalarSizeInBits()"),
             "WidenScalar",
             "LegalizeMutations::changeElementSizeTo(" + Idx + ", " + Other +
                 ")");
      return Finish();
    }

    // maxScalarEltSameAsIf: narrow to the other slot's size.  Unlike the min
    // form this copies the other slot's type wholesale, not its element size.
    bool translateMaxScalarSameAs() {
      NeedArgs(2);
      const std::string Idx = IdxOf(0);
      const std::string Other = IdxOf(1);
      EmitIf(conjunction(Guard, "Q.Types[" + Other +
                                    "].getScalarSizeInBits() < Q.Types[" + Idx +
                                    "].getScalarSizeInBits()"),
             "NarrowScalar",
             "[](const LegalityQuery &Q) { return std::make_pair(unsigned(" +
                 Idx + "), Q.Types[" + Other + "]); }");
      return Finish();
    }

    // unsupportedIfMemSizeNotPow2 / lowerIfMemSizeNotPow2 /
    // lowerIfMemSizeNotByteSizePow2: one predicate over the memory descriptor,
    // with the action the builder declares.
    bool translateMemSizePredicate() {
      if (!Args.empty())
        return Fail();
      const bool Bytes = Name == "lowerIfMemSizeNotByteSizePow2";
      EmitIf(conjunction(Guard, std::string(Bytes ? "memSizeNotByteSizePow2"
                                                  : "memSizeInBytesNotPow2") +
                                    "(0)(Q)"),
             Action);
      return Finish();
    }

    // widenScalarOrEltToNextPow2OrMinSize: overload 0 has no floor, overload 1
    // conjoins the narrowness test and passes the floor to the mutation.
    bool translateWidenScalarToNextPow2OrMinSize() {
      if (Args.empty() || Args.size() > 2)
        return Fail();
      const std::string Idx = IdxOf(0);
      if (Args.size() == 1) {
        EmitIf(conjunction(Guard, "scalarOrEltSizeNotPow2(" + Idx + ")(Q)"),
               "WidenScalar",
               "LegalizeMutations::widenScalarOrEltToNextPow2(" + Idx + ")");
        return Finish();
      }
      const std::string Min = bitsText(*Args[1]);
      EmitIf(conjunction(Guard, "(scalarOrEltNarrowerThan(" + Idx + ", " + Min +
                                    ")(Q) || scalarOrEltSizeNotPow2(" + Idx +
                                    ")(Q))"),
             "WidenScalar",
             "LegalizeMutations::widenScalarOrEltToNextPow2(" + Idx + ", " +
                 Min + ")");
      return Finish();
    }

    // widenVectorEltsToVectorMinSize: guard on the vector's total size, then
    // promote the element to VectorSize / NumElts.  Both are lambdas over the
    // query in the C++ builder, so both are written out here.
    bool translateWidenVectorEltsToVectorMinSize() {
      NeedArgs(2);
      const std::string Idx = IdxOf(0);
      const std::string Size = bitsText(*Args[1]);
      const std::string V = "Q.Types[" + Idx + "]";
      EmitIf(conjunction(Guard, V + ".isFixedVector() && " + V +
                                    ".getSizeInBits() < " + Size),
             "WidenScalar",
             "[](const LegalityQuery &Q) { const LLT VecTy = Q.Types[" + Idx +
                 "]; const unsigned NumElts = VecTy.getNumElements(); "
                 "const unsigned MinSize = " +
                 Size + " / NumElts; return std::make_pair(unsigned(" + Idx +
                 "), LLT::fixed_vector(NumElts, "
                 "VecTy.getElementType().changeElementSize(MinSize))); }");
      return Finish();
    }

    // The *If builders whose mutation is C++ text (bitcastIf, widenScalarIf,
    // narrowScalarIf, fewerElementsIf, moreElementsIf ...): the evaluator calls
    // the body on the query exactly as the chain does, so the step stays
    // concrete.
    bool translateRawMutationAction() {
      if (Guard.empty() || Args.size() != 1 ||
          !Args[0]->isSubClassOf("LegalizeRawMutation"))
        return Fail();
      EmitIf(Guard, Action, squashSpace(Args[0]->getValueAsString("Text")));
      return Finish();
    }

    /// lowerIf has a guard-only overload and a guard-plus-mutation one.
    bool translateGuardedOrRawMutation() {
      if (Args.empty())
        return translateGuardedAction();
      return translateRawMutationAction();
    }

    // widenScalarFor(Types, Mutation): the C++ builder adds one rule per type.
    bool translateWidenScalarFor() {
      if (Args.size() != 2 || !Args[1]->isSubClassOf("LegalizeRawMutation") ||
          (!Args[0]->isSubClassOf("LegalizeNamedTypeList") &&
           !Args[0]->isSubClassOf("LegalizeInlineTypeList")))
        return Fail();
      const std::string Mut = squashSpace(Args[1]->getValueAsString("Text"));
      for (const std::string &E : listElements(*Args[0]))
        EmitIf(conjunction(Guard, "typeIs(0, " + E + ")(Q)"), Action, Mut);
      return Finish();
    }

    // immIdx is a coverage marker, not a legalization step: the chain records
    // it at build time and adds no runtime rule, so the evaluator emits none.
    bool translateImmIdx() { return Finish(); }
  };

  /// One builder name the table evaluator can translate, and how.
  struct RuleTranslator {
    StringLiteral Builder;
    bool (RuleTranslation::*Translate)();
  };

  /// Keys are builderName() spellings.  A name that no builder declares would
  /// be dead code, so run() rejects it next to the registry check.
  static constexpr RuleTranslator RuleTranslators[] = {
      {"alwaysLegal", &RuleTranslation::translateBareAction},
      {"lower", &RuleTranslation::translateBareAction},
      {"libcall", &RuleTranslation::translateBareAction},
      {"custom", &RuleTranslation::translateBareAction},
      {"unsupported", &RuleTranslation::translateBareAction},
      {"legalIf", &RuleTranslation::translateGuardedAction},
      {"lowerIf", &RuleTranslation::translateGuardedOrRawMutation},
      {"customIf", &RuleTranslation::translateGuardedAction},
      {"widenScalarIf", &RuleTranslation::translateRawMutationAction},
      {"narrowScalarIf", &RuleTranslation::translateRawMutationAction},
      {"libcallIf", &RuleTranslation::translateGuardedAction},
      {"fewerElementsIf", &RuleTranslation::translateRawMutationAction},
      {"moreElementsIf", &RuleTranslation::translateRawMutationAction},
      {"unsupportedIf", &RuleTranslation::translateGuardedAction},
      {"legalFor", &RuleTranslation::translateActionFor},
      {"lowerFor", &RuleTranslation::translateActionFor},
      {"libcallFor", &RuleTranslation::translateActionFor},
      {"customFor", &RuleTranslation::translateActionFor},
      {"legalForCartesianProduct", &RuleTranslation::translateCartesian},
      {"customForCartesianProduct", &RuleTranslation::translateCartesian},
      {"legalForTypesWithMemDesc", &RuleTranslation::translateMemDesc},
      {"clampScalar", &RuleTranslation::translateClampScalar},
      {"clampScalarOrElt", &RuleTranslation::translateClampScalar},
      {"minScalarIf", &RuleTranslation::translateScalarIf},
      {"maxScalarIf", &RuleTranslation::translateScalarIf},
      {"minScalar", &RuleTranslation::translateMinMaxScalar},
      {"maxScalar", &RuleTranslation::translateMinMaxScalar},
      {"scalarSameSizeAs", &RuleTranslation::translateScalarSameSizeAs},
      {"scalarize", &RuleTranslation::translateScalarize},
      {"scalarizeIf", &RuleTranslation::translateScalarize},
      {"moreElementsToNextPow2",
       &RuleTranslation::translateMoreElementsToNextPow2},
      {"widenScalarToNextPow2",
       &RuleTranslation::translateWidenScalarToNextPow2},
      {"widenScalarOrEltToNextPow2",
       &RuleTranslation::translateWidenScalarToNextPow2},
      {"clampNumElements", &RuleTranslation::translateClampNumElements},
      {"clampMinNumElements",
       &RuleTranslation::translateClampMinMaxNumElements},
      {"clampMaxNumElements",
       &RuleTranslation::translateClampMinMaxNumElements},
      {"bitcastIf", &RuleTranslation::translateRawMutationAction},
      {"widenScalarFor", &RuleTranslation::translateWidenScalarFor},
      {"unsupportedFor", &RuleTranslation::translateUnsupportedFor},
      {"minScalarOrElt", &RuleTranslation::translateMinScalarOrElt},
      {"minScalarOrEltIf", &RuleTranslation::translateMinScalarOrElt},
      {"minScalarSameAs", &RuleTranslation::translateMinScalarSameAs},
      {"minScalarEltSameAsIf", &RuleTranslation::translateMinScalarSameAs},
      {"maxScalarEltSameAsIf", &RuleTranslation::translateMaxScalarSameAs},
      {"unsupportedIfMemSizeNotPow2",
       &RuleTranslation::translateMemSizePredicate},
      {"lowerIfMemSizeNotPow2", &RuleTranslation::translateMemSizePredicate},
      {"lowerIfMemSizeNotByteSizePow2",
       &RuleTranslation::translateMemSizePredicate},
      {"widenScalarOrEltToNextPow2OrMinSize",
       &RuleTranslation::translateWidenScalarToNextPow2OrMinSize},
      {"widenVectorEltsToVectorMinSize",
       &RuleTranslation::translateWidenVectorEltsToVectorMinSize},
      {"immIdx", &RuleTranslation::translateImmIdx},
  };

  static const RuleTranslator *lookupRuleTranslator(StringRef Name) {
    for (const RuleTranslator &T : RuleTranslators)
      if (T.Builder == Name)
        return &T;
    return nullptr;
  }

  /// Translate one rule into the statements an evaluator runs.  Returns false
  /// when the builder has no table translation; the whole rule set is then
  /// opaque and the reader falls back to the chain for it.
  /// With RuleIdx set the emitted statements store the step and the rule's
  /// position instead of returning it, for -gen-legalizer-rule-index.
  /// With GuardOnly set the emitted statements test the rule's guard and
  /// return true on a match instead of applying the step.  That is the
  /// condition the chain tests before it commits to the rule, with no
  /// mutation, for the rule-index backend's per-rule guard evaluators.
  bool translateRule(const Record &Rule, std::string &Out, std::string &Why,
                     const unsigned *RuleIdx = nullptr,
                     bool GuardOnly = false) {
    if (isCoreRule(Rule))
      return translateCoreRule(Rule, Out, Why, RuleIdx, GuardOnly);
    RuleTranslation T(Rule, TablePrefix, Out, Why, RuleIdx, GuardOnly);
    if (!Rule.getValueAsString("OpaqueArgs").empty())
      return T.translateOpaqueBody();
    const RuleTranslator *H = lookupRuleTranslator(T.Name);
    if (!H)
      return T.Fail();
    return (T.*H->Translate)();
  }

  /// The guard a core rule carries: the legacy Pred record or the typed
  /// CoreGuard, in the spelling the evaluator uses.
  static std::string coreRuleGuardText(const Record &Rule,
                                       const RecordKeeper &Records,
                                       StringRef QueryVar) {
    if (const Record *P = Rule.getValueAsOptionalDef("Pred")) {
      std::string S;
      raw_string_ostream OS(S);
      renderPredicate(*P, OS);
      OS.flush();
      if (P->isSubClassOf("LegalizeQueryPredicateBase"))
        return "(" + S + ")(Q)";
      return S;
    }
    const Init *G = Rule.getValueInit("CoreGuard");
    if (G && !isa<UnsetInit>(G)) {
      std::unique_ptr<CoreNode> N = parseCoreNode(*G, Rule);
      return CoreCxxEmitter(Records, Rule, QueryVar).boolean(*N);
    }
    return "";
  }

  /// Translate a core rule into the statements its shim would run.  Only
  /// primitives with a table translation are accepted; a new primitive must
  /// be translated here rather than silently dropping the rule set back to
  /// the chain, so the fatal is deliberate.
  bool translateCoreRule(const Record &Rule, std::string &Out, std::string &Why,
                         const unsigned *RuleIdx, bool GuardOnly) {
    CheckedCoreRule C = checkCoreRule(Rule, Records);
    CoreCxxEmitter Cxx(Records, Rule, "Q");
    StringRef On = C.Obs ? coreObservationForm(*C.Obs) : StringRef();
    StringRef To = C.Target ? coreTag(*C.Target->Op) : StringRef();
    StringRef Op = coreTag(*C.Operator);
    const CoreShimRow *Shim = lookupCoreShim(Op, On, To);
    if (!Shim)
      PrintFatalError(Rule.getLoc(),
                      "core rule: the table backends do not translate "
                      "operator '" +
                          Op + "' over '" + On + "' to '" + To + "' yet");
    const std::string P = coreRuleGuardText(Rule, Records, "Q");
    const std::string Idx = std::to_string(C.TypeIdx);
    const std::string V = "Q.Types[" + Idx + "]";
    std::string Cond;
    std::string Mut;
    switch (Shim->Table) {
    case CoreTableTranslation::None:
      PrintFatalError(Rule.getLoc(),
                      "core rule: the table backends do not translate shim '" +
                          Shim->Method + "' yet");
      llvm_unreachable("PrintFatalError returned");
    case CoreTableTranslation::WidenVectorMinSize: {
      const std::string Size = std::to_string(C.Target->Kids[0]->IntValue);
      Cond = "(" + V + ".isFixedVector() && " + V + ".getSizeInBits() < " +
             Size + ")";
      Mut = "[](const LegalityQuery &Q) { const LLT VecTy = Q.Types[" + Idx +
            "]; const unsigned NumElts = VecTy.getNumElements(); "
            "const unsigned MinSize = " +
            Size + " / NumElts; return std::make_pair(unsigned(" + Idx +
            "), LLT::fixed_vector(NumElts, "
            "VecTy.getElementType().changeElementSize(MinSize))); }";
      break;
    }
    case CoreTableTranslation::GuardedAction:
      // The guard is the whole rule: the action needs no type index and no
      // mutation, so both stay empty and emitStep() writes the return.
      break;
    case CoreTableTranslation::Scalarize:
      // Same condition and mutation as the chain's scalarizeIf(): the vector
      // test is part of the action, not of the guard.
      Cond = "isVector(" + Idx + ")(Q)";
      Mut = "LegalizeMutations::scalarize(" + Idx + ")";
      break;
    case CoreTableTranslation::MinScalarOrElt: {
      const std::string Ty = Cxx.value(*C.Target);
      Cond = "scalarOrEltNarrowerThan(" + Idx + ", (" + Ty +
             ").getScalarSizeInBits())(Q)";
      Mut = "LegalizeMutations::changeElementSizeTo(" + Idx + ", " + Ty + ")";
      break;
    }
    case CoreTableTranslation::WidenPow2OrMinSize: {
      const std::string Min = Cxx.value(*C.Target);
      Cond = "(scalarOrEltNarrowerThan(" + Idx + ", " + Min +
             ")(Q) || "
             "scalarOrEltSizeNotPow2(" +
             Idx + ")(Q))";
      Mut = "LegalizeMutations::widenScalarOrEltToNextPow2(" + Idx + ", " +
            Min + ")";
      break;
    }
    }
    Cond = conjunction(P, Cond);
    std::string Buf;
    raw_string_ostream OS(Buf);
    if (Cond.empty()) {
      // The rule always matches, so there is nothing to test.  An action with
      // no mutation still has to be returned, and emitStep() does that.
      if (GuardOnly) {
        OS << "  return true;\n";
      } else {
        const std::string RuleIdxText =
            RuleIdx ? std::to_string(*RuleIdx) : std::string();
        const std::string MutationsFlag =
            RuleIdx ? (std::string(TablePrefix) + "EvalRuleIndexMutations")
                    : std::string();
        emitStep(OS, C.Action->getValueAsString("Name"), "  ", Mut, RuleIdxText,
                 MutationsFlag);
      }
      OS.flush();
      Out = std::move(Buf);
      return true;
    }
    OS << "  if (" << Cond << ") {\n";
    if (GuardOnly) {
      OS << "    return true;\n";
    } else {
      const std::string RuleIdxText =
          RuleIdx ? std::to_string(*RuleIdx) : std::string();
      const std::string MutationsFlag =
          RuleIdx ? (std::string(TablePrefix) + "EvalRuleIndexMutations")
                  : std::string();
      emitStep(OS, C.Action->getValueAsString("Name"), "    ", Mut, RuleIdxText,
               MutationsFlag);
    }
    OS << "  }\n";
    OS.flush();
    Out = std::move(Buf);
    return true;
  }

  /// True when the rule set's last rule always matches.  The evaluator's final
  /// Unsupported return would then be dead code, so it is left out.
  static bool endsUnconditional(const Record &RuleSet) {
    std::vector<const Record *> Rules = RuleSet.getValueAsListOfDefs("Rules");
    if (Rules.empty())
      return false;
    const Record &Last = *Rules.back();
    if (isCoreRule(Last) ||
        !Last.getValueAsString("OpaqueArgs").empty() ||
        Last.getValueAsOptionalDef("Pred") ||
        !Last.getValueAsListOfDefs("Args").empty())
      return false;
    StringRef Name = builderName(Last);
    return Name == "alwaysLegal" || Name == "lower" || Name == "libcall" ||
           Name == "custom" || Name == "unsupported";
  }

public:
  explicit LegalizerTableEmitterImpl(const RecordKeeper &RK) : Records(RK) {
    for (const Record *T :
         Records.getAllDerivedDefinitions("LegalizeNamedType"))
      NamedTypes[T->getName().str()] = T;
  }

  void run(raw_ostream &OS) {
    verifyLoweringRegistry(Records);
    // The translation table is keyed by builder name.  A duplicate key, or a
    // key no LegalizeBuilder declares, is dead code that the reader would
    // silently never reach, so reject it before emitting anything.
    std::set<StringRef> UnusedTranslators;
    for (const RuleTranslator &T : RuleTranslators)
      if (!UnusedTranslators.insert(T.Builder).second)
        PrintFatalError("duplicate table translation for builder '" +
                        StringRef(T.Builder) + "'");
    for (const Record *B : Records.getAllDerivedDefinitions("LegalizeBuilder"))
      UnusedTranslators.erase(B->getValueAsString("Name"));
    for (const StringRef &Name : UnusedTranslators)
      PrintFatalError("table translation for unknown builder '" + Name + "'");

    collectClasses();
    collectMemClasses();

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

    OS << "//===-- " << P
       << "LegalizerTable.inc - generated table data "
          "-----*- C++ -*-===//\n";
    OS << "//\n// Generated by llvm-tblgen -gen-legalizer-table.  Do not "
          "edit.\n";
    OS << "// Type classes, per-opcode descriptors and the per-rule-set "
          "evaluators.\n";
    OS << "//\n//"
          "===-----------------------------------------------------------------"
          "-----===//\n\n";

    emitFeatureSignature(OS);

    OS << "static constexpr LegalizerTypeClass " << P << "TypeClasses[] = {\n";
    for (size_t I = 0; I != Classes.size(); ++I) {
      const TableType &C = Classes[I];
      OS << "  /*" << I << "*/ {" << tableTypeKindName(C.Kind) << ", "
         << C.ElemBits << ", " << C.Lanes << "}, // " << C.Name << "\n";
    }
    OS << "};\n";
    OS << "static constexpr unsigned " << P << "NumTypeClasses = sizeof(" << P
       << "TypeClasses) / sizeof(" << P << "TypeClasses[0]);\n\n";

    // The descriptor's memory type is matched by identity, so it has its own
    // class list.
    OS << "// Classes of the memory-descriptor axis (the descriptor's LLT).\n";
    OS << "static constexpr LegalizerTypeClass " << P << "MemClasses[] = {\n";
    for (size_t I = 0; I != MemClasses.size(); ++I) {
      const TableType &C = MemClasses[I];
      OS << "  /*" << I << "*/ {" << tableTypeKindName(C.Kind) << ", "
         << C.ElemBits << ", " << C.Lanes << "}, // " << C.Name << "\n";
    }
    OS << "};\n";
    OS << "static constexpr unsigned " << P << "NumMemClasses = sizeof(" << P
       << "MemClasses) / sizeof(" << P << "MemClasses[0]);\n\n";

    // A memory descriptor is a separate axis: the type slots stay 0 and 1,
    // and a rule set that reads MMODescr[0] keys on the descriptor too.
    std::vector<unsigned> Aligns = alignThresholds();
    std::string AlignList;
    for (unsigned A : Aligns) {
      if (!AlignList.empty())
        AlignList += ", ";
      AlignList += std::to_string(A);
    }
    OS << "// Alignment classes for the memory-descriptor axis: a query meets "
          "a\n"
          "// prefix of the thresholds, because TypePairAndMemDesc only asks\n"
          "// Align >= Entry.Align.\n";
    OS << "static constexpr unsigned " << P << "AlignThresholds[] = {"
       << AlignList << "};\n";
    OS << "static constexpr unsigned " << P << "NumAlignClasses = sizeof(" << P
       << "AlignThresholds) / sizeof(" << P << "AlignThresholds[0]) + 1;\n\n";

    OS << "// Slots 0 and 1, partitioned by the full class list; a rule set "
          "that\n"
          "// reads a memory descriptor adds the descriptor axis.  A rule set\n"
          "// that needs anything else is not in this table (see the "
          "emitter).\n";
    OS << "static constexpr LegalizerTableDesc " << P << "Tables[] = {\n";
    for (size_t I = 0; I != Tables.size(); ++I) {
      const bool HasMem = needsMemDesc(*Tables[I]);
      OS << "  /*" << I << "*/ {{0, 1}, {" << Classes.size() << ", "
         << Classes.size() << "}, " << (HasMem ? "0" : "kNoMemDesc") << ", "
         << (HasMem ? P + "NumMemClasses * " + P + "NumAlignClasses" : "0")
         << "}, // " << Tables[I]->getName() << "\n";
    }
    OS << "};\n";
    OS << "static constexpr unsigned " << P << "NumTables = sizeof(" << P
       << "Tables) / sizeof(" << P << "Tables[0]);\n\n";

    OS << "static inline int " << Lower
       << "TableForOpcode(unsigned Opcode) {\n";
    OS << "  switch (Opcode) {\n";
    for (size_t I = 0; I != Tables.size(); ++I)
      for (StringRef Opcode : Tables[I]->getValueAsListOfStrings("Opcodes"))
        OS << "  case TargetOpcode::" << Opcode << ": return " << I << ";\n";
    OS << "  default: return -1;\n  }\n}\n";

    // The evaluators are the rule chain with its guards and mutations written
    // out.  They are keyed like the descriptors above, so a table answer is a
    // property of the .td rules, not of a second description of them.
    std::vector<bool> Opaque(Tables.size(), false);
    std::vector<bool> Tail(Tables.size(), false);
    std::vector<std::string> Why(Tables.size());
    std::vector<std::string> Bodies(Tables.size());
    for (size_t I = 0; I != Tables.size(); ++I) {
      for (const Record *Rule : Tables[I]->getValueAsListOfDefs("Rules")) {
        std::string Text, Reason;
        if (!translateRule(*Rule, Text, Reason)) {
          Opaque[I] = true;
          Why[I] = Reason;
          Bodies[I].clear();
          break;
        }
        Bodies[I] += Text;
      }
      Tail[I] = !Opaque[I] && endsUnconditional(*Tables[I]);
    }

    OS << "\n// One evaluator per tabulated rule set.  Each begins with the "
          "same prologue\n// -gen-legalizer emits: the feature bools and LLT "
          "constants the .td rules\n// are written in terms of, so the table "
          "cannot disagree with the chain\n// about what a type or a feature "
          "means.\n";
    for (size_t I = 0; I != Tables.size(); ++I) {
      OS << "\n";
      if (Opaque[I]) {
        OS << "// " << Tables[I]->getName() << " is opaque: " << Why[I]
           << ".\n// The reader falls back to the rule chain for this rule "
              "set.\n";
        continue;
      }
      OS << "static LegalizeActionStep " << P << "Eval" << I << "(\n";
      OS << "    const " << P
         << "Subtarget &Subtarget, const LegalityQuery &Q) {\n";
      EmitPrologue(OS);
      OS << Bodies[I];
      if (!Tail[I])
        OS << "  return {LegalizeActions::Unsupported, 0, LLT{}};\n";
      OS << "}\n";
    }

    OS << "\n/// Answer one query from a tabulated rule set.  False means "
          "the rule set\n/// is opaque, and the caller uses the rule chain "
          "instead.\n";
    OS << "static std::optional<LegalizeActionStep> " << P
       << "EvalTable(unsigned Table, const " << P
       << "Subtarget &Subtarget,\n"
          "                         const LegalityQuery &Q) {\n";
    OS << "  switch (Table) {\n";
    for (size_t I = 0; I != Tables.size(); ++I) {
      OS << "  case " << I << ": // " << Tables[I]->getName() << "\n";
      if (Opaque[I]) {
        OS << "    return std::nullopt;\n";
      } else {
        OS << "    return " << P << "Eval" << I << "(Subtarget, Q);\n";
      }
    }
    OS << "  default:\n    return std::nullopt;\n  }\n}\n";
  }

  /// The test-only rule-index output (-gen-legalizer-rule-index).
  ///
  /// The evaluators are the same translation of the .td rules as run() emits:
  /// same prologue, same guards, same mutations.  The one difference is the
  /// match form: instead of returning the step, a rule stores it in Out, names
  /// itself by its 0-based position in the rule set's Rules list, and reports
  /// a match.  The numbering is the one X86LegalizerInfo.core.json records as
  /// rule_index, so a test can hold the two side by side.
  ///
  /// Nothing here is part of the table reader: no production TU includes this
  /// file, and the table the target builds is the one run() emits.
  void runRuleIndex(raw_ostream &OS) {
    verifyLoweringRegistry(Records);
    collectClasses();
    collectMemClasses();

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

    OS << "//===-- " << P
       << "LegalizerRuleIndex.inc - first matching rule "
          "-----*- C++ -*-===//\n";
    OS << "//\n// Generated by llvm-tblgen -gen-legalizer-rule-index.  Do not "
          "edit.\n";
    OS << "//\n// TEST-ONLY.  " << P
       << "LegalizerTable.inc does not include this file and no\n"
          "// production TU compiles it.  Each evaluator is the same "
          "translation of the\n"
          "// .td rules as the table's, but it also reports which rule "
          "matched.\n";
    OS << "//\n//"
          "===-----------------------------------------------------------------"
          "-----===//\n\n";

    OS << "// Test-only switch: a match normally stores its concrete step, "
          "but\n"
          "// a domain point the real chain would never see (for example a\n"
          "// pointer in a scalar mutation slot) can assert while evaluating "
          "a\n"
          "// mutation.  A caller that only needs the rule identity clears "
          "it.\n";
    OS << "static bool " << P << "EvalRuleIndexMutations = true;\n\n";

    // Translate every rule, keeping its position in the rule set.  A rule
    // whose builder has no table translation makes its whole rule set opaque,
    // exactly as it does for the table.
    std::vector<bool> Opaque(Tables.size(), false);
    std::vector<std::string> Why(Tables.size());
    std::vector<std::string> Bodies(Tables.size());
    std::vector<unsigned> NumRules(Tables.size(), 0);
    for (size_t I = 0; I != Tables.size(); ++I) {
      std::vector<const Record *> Rules =
          Tables[I]->getValueAsListOfDefs("Rules");
      NumRules[I] = Rules.size();
      for (unsigned R = 0; R != Rules.size(); ++R) {
        std::string Text, Reason;
        if (!translateRule(*Rules[R], Text, Reason, &R)) {
          Opaque[I] = true;
          Why[I] = Reason;
          Bodies[I].clear();
          break;
        }
        Bodies[I] += Text;
      }
    }

    OS << "\n// One evaluator per tabulated rule set, in X86Tables[] order.  ";
    OS << "A match\n// stores its step in Out and its rule's position in "
          "RuleIndex.\n";
    for (size_t I = 0; I != Tables.size(); ++I) {
      OS << "\n";
      if (Opaque[I]) {
        OS << "// " << Tables[I]->getName() << " is opaque: " << Why[I]
           << ".\n// The table's evaluator declines this rule set as well.\n";
        OS << "static bool " << P << "EvalRuleIndex" << I << "(\n";
        OS << "    const " << P << "Subtarget &, const LegalityQuery &,\n";
        OS << "    unsigned &, LegalizeActionStep &) {\n  return false;\n}\n";
        continue;
      }
      OS << "static bool " << P << "EvalRuleIndex" << I << "(\n";
      OS << "    const " << P
         << "Subtarget &Subtarget, const LegalityQuery &Q,\n";
      OS << "    unsigned &RuleIndex, LegalizeActionStep &Out) {\n";
      EmitPrologue(OS);
      OS << Bodies[I];
      OS << "  return false;\n}\n";
    }

    // One guard evaluator per rule, named by (rule set, position).  The body
    // is the condition the chain tests before committing to the rule; the
    // step and its mutation are omitted, so probing a shape the real chain
    // would never see cannot assert while a test looks for a trigger.
    OS << "\n// One guard evaluator per rule, named by (table, position).  "
          "The body is\n// the rule's firing condition with no mutation "
          "applied.\n";
    for (size_t I = 0; I != Tables.size(); ++I) {
      std::vector<const Record *> Rules =
          Tables[I]->getValueAsListOfDefs("Rules");
      for (unsigned R = 0; R != Rules.size(); ++R) {
        std::string Text, Reason;
        OS << "static bool " << P << "EvalRuleGuard" << I << "_" << R
           << "(const " << P
           << "Subtarget &Subtarget,\n"
              "    const LegalityQuery &Q) {\n";
        EmitPrologue(OS);
        if (translateRule(*Rules[R], Text, Reason, nullptr, true))
          OS << Text;
        else
          OS << "  // " << Reason << "\n";
        OS << "  return false;\n}\n";
      }
    }

    // The dispatch the test calls: (table, position) is the pair the Core IR
    // records as (def, rule_index), so a test can name a rule without
    // re-deriving its position.
    OS << "\n/// Evaluate one rule's guard; false for an unknown rule.\n";
    OS << "static bool " << P
       << "EvalRuleGuard(unsigned Table, unsigned RuleIndex,\n"
          "                          const "
       << P
       << "Subtarget &Subtarget,\n"
          "                          const LegalityQuery &Q) {\n";
    OS << "  switch (Table) {\n";
    for (size_t I = 0; I != Tables.size(); ++I) {
      OS << "  case " << I << ": // " << Tables[I]->getName() << "\n";
      OS << "    switch (RuleIndex) {\n";
      for (unsigned R = 0; R != NumRules[I]; ++R)
        OS << "    case " << R << ": return " << P << "EvalRuleGuard" << I
           << "_" << R << "(Subtarget, Q);\n";
      OS << "    default: return false;\n    }\n";
    }
    OS << "  default: return false;\n  }\n}\n";

    // The test's own map from opcode to rule set, plus the rule counts the
    // index is bounded by.  The reader keys by class and never needs either.
    OS << "\n// Opcode to rule set, in X86Tables[] order, and the number of "
          ".td rules\n// in each set.  A matched index is in [0, "
          "NumRules[Table]).\n";
    OS << "static constexpr const char *" << P
       << "RuleIndexTableNames[] = "
          "{\n";
    for (const Record *RS : Tables)
      OS << "  \"" << RS->getName() << "\",\n";
    OS << "};\n";
    OS << "static constexpr unsigned " << P << "RuleIndexNumRules[] = {\n";
    for (unsigned N : NumRules)
      OS << "  " << N << ",\n";
    OS << "};\n";
    OS << "static int " << Lower
       << "RuleIndexTableForOpcode(unsigned Opcode) {\n";
    OS << "  switch (Opcode) {\n";
    for (size_t I = 0; I != Tables.size(); ++I)
      for (StringRef Opcode : Tables[I]->getValueAsListOfStrings("Opcodes"))
        OS << "  case TargetOpcode::" << Opcode << ": return " << I << ";\n";
    OS << "  default: return -1;\n  }\n}\n";

    // The domain enumeration needs the axes and the per-table shape, not just
    // the evaluators.  Emit them from the same data run() uses, so a class
    // cannot be added to the table and silently omitted from the test domain.
    OS << "\n// TEST-ONLY domain metadata.  The type axis, the descriptor "
          "axis and the\n// shape of every table, from the same data the "
          "reader's arrays use.  The\n// test enumerates the domain from "
          "these, never from a copy.\n";
    OS << "enum " << P << "TestTypeKind : uint8_t {\n";
    OS << "  " << P << "TestKind_Pointer = 0,\n";
    OS << "  " << P << "TestKind_Scalar = 1,\n";
    OS << "  " << P << "TestKind_Vector = 2,\n";
    OS << "  " << P << "TestKind_PtrVector = 3,\n";
    OS << "};\n";
    OS << "struct " << P << "TestTypeClass {\n";
    OS << "  uint8_t Kind;\n";
    OS << "  uint16_t ElemBits;\n";
    OS << "  uint16_t Lanes;\n";
    OS << "};\n";
    OS << "struct " << P << "TestTable {\n";
    OS << "  const char *Name;\n";
    OS << "  const char *Opcode;\n";
    OS << "  uint8_t Slots[2];\n";
    OS << "  uint8_t Dims[2];\n";
    OS << "  bool HasMem;\n";
    OS << "  uint16_t MemDim;\n";
    OS << "  uint16_t NumRules;\n";
    OS << "  uint8_t NumSlots;\n";
    OS << "};\n\n";

    OS << "static constexpr " << P << "TestTypeClass " << P
       << "TestTypeClasses[] = {\n";
    for (size_t I = 0; I != Classes.size(); ++I) {
      const TableType &C = Classes[I];
      OS << "  /*" << I << "*/ {" << testTypeKindName(C.Kind, P) << ", "
         << C.ElemBits << ", " << C.Lanes << "}, // " << C.Name << "\n";
    }
    OS << "};\n";
    OS << "static constexpr unsigned " << P << "TestNumTypeClasses = sizeof("
       << P << "TestTypeClasses) / sizeof(" << P << "TestTypeClasses[0]);\n\n";

    OS << "static constexpr " << P << "TestTypeClass " << P
       << "TestMemClasses[] = {\n";
    for (size_t I = 0; I != MemClasses.size(); ++I) {
      const TableType &C = MemClasses[I];
      OS << "  /*" << I << "*/ {" << testTypeKindName(C.Kind, P) << ", "
         << C.ElemBits << ", " << C.Lanes << "}, // " << C.Name << "\n";
    }
    OS << "};\n";
    OS << "static constexpr unsigned " << P << "TestNumMemClasses = sizeof("
       << P << "TestMemClasses) / sizeof(" << P << "TestMemClasses[0]);\n\n";

    std::vector<unsigned> Aligns = alignThresholds();
    OS << "static constexpr unsigned " << P << "TestAlignThresholds[] = {";
    for (size_t I = 0; I != Aligns.size(); ++I) {
      if (I)
        OS << ", ";
      OS << Aligns[I];
    }
    OS << "};\n";
    OS << "static constexpr unsigned " << P << "TestNumAlignClasses = sizeof("
       << P << "TestAlignThresholds) / sizeof(" << P
       << "TestAlignThresholds[0]) + 1;\n\n";

    OS << "static constexpr " << P << "TestTable " << P
       << "TestTables[] = "
          "{\n";
    for (size_t I = 0; I != Tables.size(); ++I) {
      const bool HasMem = needsMemDesc(*Tables[I]);
      const std::vector<StringRef> Opcodes =
          Tables[I]->getValueAsListOfStrings("Opcodes");
      if (Opcodes.empty())
        PrintFatalError(Tables[I]->getLoc(),
                        "a tabulated rule set must name an opcode");
      OS << "  /*" << I << "*/ {\"" << Tables[I]->getName() << "\", \""
         << Opcodes.front() << "\", {0, 1}, {" << Classes.size() << ", "
         << Classes.size() << "}, " << (HasMem ? "true" : "false") << ", "
         << (HasMem ? MemClasses.size() * (Aligns.size() + 1) : 0) << ", "
         << NumRules[I] << ", " << testArityOf(*Tables[I]) << "}, // "
         << Tables[I]->getName() << "\n";
    }
    OS << "};\n";
    OS << "static constexpr unsigned " << P << "TestNumTables = sizeof(" << P
       << "TestTables) / sizeof(" << P << "TestTables[0]);\n";

    OS << "\n/// Answer one query from a tabulated rule set and report the "
          "first\n/// matching rule.  False means no rule matched, or the "
          "rule set is opaque.\n";
    OS << "static bool " << P << "EvalRuleIndex(unsigned Table, const " << P
       << "Subtarget &Subtarget,\n"
          "                             const LegalityQuery &Q, unsigned "
          "&RuleIndex,\n"
          "                             LegalizeActionStep &Out) {\n";
    OS << "  switch (Table) {\n";
    for (size_t I = 0; I != Tables.size(); ++I)
      OS << "  case " << I << ": // " << Tables[I]->getName() << "\n"
         << "    return " << P << "EvalRuleIndex" << I
         << "(Subtarget, Q, RuleIndex, Out);\n";
    OS << "  default:\n    return false;\n  }\n}\n";
  }

  /// Emit the environment the table is specialized on: one bit per
  /// LegalizeFeature record, in .td order, plus the function that reads them
  /// off a subtarget.  The table is a function of exactly these features (and
  /// the pointer width), so two subtargets with equal signatures must answer
  /// every query identically.  Emitting the bits from the same records the
  /// evaluators use keeps the signature from drifting when a feature is added.
  void emitFeatureSignature(raw_ostream &OS) {
    std::vector<const Record *> Features =
        Records.getAllDerivedDefinitions("LegalizeFeature");
    llvm::sort(Features, LessRecordByID());
    if (Features.size() > 32)
      PrintFatalError("feature signature needs more than 32 bits");

    OS << "// Environment signature: one bit per LegalizeFeature, in .td "
          "order.\n"
          "// A shared table cache keys on this, so equal signatures must "
          "mean equal\n"
          "// answers.\n";
    OS << "enum : uint32_t {\n";
    for (size_t I = 0; I != Features.size(); ++I)
      OS << "  " << TablePrefix << "FeatureBit_" << Features[I]->getName()
         << " = 1u << " << I << ",\n";
    OS << "};\n\n";

    OS << "static uint32_t " << TablePrefix
       << "FeatureSignature(const " << TablePrefix
       << "Subtarget &Subtarget) {\n"
          "  uint32_t S = 0;\n";
    for (const Record *F : Features)
      OS << "  S |= (" << squashSpace(F->getValueAsString("Cond")) << ") ? "
         << TablePrefix << "FeatureBit_" << F->getName() << " : 0u;\n";
    OS << "  return S;\n}\n\n";
  }

  /// Emit the identifiers a rule, an evaluator and -gen-legalizer all refer to:
  /// the feature bools, the LLT constants and the named type lists.  The
  /// -gen-legalizer emitter filters the features down to the ones it uses; an
  /// evaluator declares all of them, so every one is marked maybe_unused and
  /// the .td order is preserved (a type is named by the record's identifier,
  /// so a type declared below its first use would not compile).
  void EmitPrologue(raw_ostream &OS) {
    std::vector<const Record *> Features =
        Records.getAllDerivedDefinitions("LegalizeFeature");
    llvm::sort(Features, LessRecordByID());
    for (const Record *F : Features)
      OS << "  [[maybe_unused]] const bool " << F->getName() << " = "
         << squashSpace(F->getValueAsString("Cond")) << ";\n";

    std::vector<const Record *> Types =
        Records.getAllDerivedDefinitions("LegalizeNamedType");
    llvm::sort(Types, LessRecordByID());
    for (const Record *T : Types) {
      OS << "  [[maybe_unused]] const LLT " << T->getName() << " = ";
      if (T->isSubClassOf("LegalizeSelectType"))
        OS << squashSpace(selectText(*T, "Types", "Otherwise"));
      else
        OS << squashSpace(T->getValueAsString("Expr"));
      OS << ";\n";
    }

    std::vector<const Record *> Lists =
        Records.getAllDerivedDefinitions("LegalizeNamedTypeList");
    llvm::sort(Lists, LessRecordByID());
    for (const Record *L : Lists) {
      SmallVector<StringRef, 8> Elems;
      for (const Record *E : L->getValueAsListOfDefs("Elems"))
        Elems.push_back(E->getName());
      OS << "  [[maybe_unused]] const std::initializer_list<LLT> "
         << L->getName() << " = {" << join(Elems, ", ") << "};\n";
    }
    OS << "\n";
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
      if (isCoreRule(*Rule)) {
        scanCoreRuleSlots(*Rule, Arity);
        continue;
      }
      const std::vector<const Record *> Args =
          Rule->getValueAsListOfDefs("Args");
      // Every *ForCartesianProduct builder takes one type slot per list, so a
      // product of two lists is a two-slot query even though no single
      // argument names slot 1.  Missing this made G_VAARG a one-slot rule set
      // whose customForCartesianProduct predicate reads Query.Types[1].
      if (builderName(*Rule).ends_with("ForCartesianProduct")) {
        unsigned Lists = 0;
        for (const Record *Arg : Args)
          if (Arg->isSubClassOf("LegalizeNamedTypeList") ||
              Arg->isSubClassOf("LegalizeInlineTypeList") ||
              Arg->isSubClassOf("LegalizeSelectTypeList"))
            ++Lists;
        Arity = std::max(Arity, Lists);
      }
      for (const Record *Arg : Args) {
        if (Arg->isSubClassOf("LegalizeArgIndex"))
          Arity = std::max(Arity, unsigned(Arg->getValueAsInt("Value") + 1));
        else if (Arg->isSubClassOf("LegalizeTypePair") ||
                 Arg->isSubClassOf("LegalizeMemDesc"))
          Arity = std::max(Arity, 2u);
      }
      scanPredicate(Rule->getValueAsOptionalDef("Pred"), Arity);
      scanSlots(Rule->getValueAsString("OpaqueArgs"), Arity);
    }
    return std::min(Arity, 3u);
  }

  /// Slot numbers in a rendered predicate or mutation.  Unlike scanSlots this
  /// accepts a call with one numeric argument, e.g. sizeNotPow2(1), because
  /// the domain metadata must see every slot a rule reads, not only the ones a
  /// comma-separated call spells.
  static void scanSlotNumbers(StringRef S, unsigned &Arity) {
    for (size_t I = 0; I < S.size(); ++I) {
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
      if (J < S.size() && (S[J] == ',' || S[J] == ')'))
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

  static void scanPredicateSlots(const Record *P, unsigned &Arity) {
    if (!P)
      return;
    if (P->isSubClassOf("LegalizeTypePredicate"))
      scanSlotNumbers(P->getValueAsString("Call"), Arity);
    else if (P->isSubClassOf("LegalizeQueryPredicate"))
      scanSlotNumbers(P->getValueAsString("Expr"), Arity);
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
            scanPredicateSlots(Def->getDef(), Arity);
      };
      Walk(P->getValueAsDag("CondDag"));
    }
  }

  /// The number of type slots a rule set's rules actually read, including the
  /// two slots a LegalizeTypePair or a LegalizeMemDesc names.  arityOf() feeds
  /// the point emitter and is deliberately conservative about the latter; the
  /// domain metadata needs the exact split between one-slot and two-slot rule
  /// sets.
  static unsigned testArityOf(const Record &RuleSet) {
    unsigned Arity = 1;
    for (const Record *Rule : RuleSet.getValueAsListOfDefs("Rules")) {
      if (isCoreRule(*Rule)) {
        scanCoreRuleSlots(*Rule, Arity);
        continue;
      }
      const std::vector<const Record *> Args =
          Rule->getValueAsListOfDefs("Args");
      // legalForCartesianProduct gives one type slot per type-list argument,
      // so a product of two lists is a two-slot query even though no single
      // argument names slot 1.
      if (builderName(*Rule).ends_with("ForCartesianProduct")) {
        unsigned Lists = 0;
        for (const Record *Arg : Args)
          if (Arg->isSubClassOf("LegalizeNamedTypeList") ||
              Arg->isSubClassOf("LegalizeInlineTypeList") ||
              Arg->isSubClassOf("LegalizeSelectTypeList"))
            ++Lists;
        Arity = std::max(Arity, Lists);
      }
      for (const Record *Arg : Args) {
        if (Arg->isSubClassOf("LegalizeArgIndex"))
          Arity = std::max(Arity, unsigned(Arg->getValueAsInt("Value") + 1));
        else if (Arg->isSubClassOf("LegalizeTypePair") ||
                 Arg->isSubClassOf("LegalizeMemDesc"))
          Arity = std::max(Arity, 2u);
      }
      scanPredicateSlots(Rule->getValueAsOptionalDef("Pred"), Arity);
      scanSlotNumbers(Rule->getValueAsString("OpaqueArgs"), Arity);
    }
    return std::min(Arity, 3u);
  }

  void runPoints(raw_ostream &OS) {
    verifyLoweringRegistry(Records);
    std::vector<const Record *> RuleSets =
        Records.getAllDerivedDefinitions("LegalizeRuleSet");
    llvm::sort(RuleSets, LessRecordByID());

    struct DomainPoint {
      std::string Text;
      unsigned NumTypes;
      std::string Mem;
    };

    // Types the class list does not name, used to probe the reader's residual
    // path for every rule set.
    static const TableType ResidualTypes[] = {
        {TableScalar, 12, 1, "residual s12"},
        {TableScalar, 27, 1, "residual s27"},
        {TableScalar, 35, 1, "residual s35"},
        {TableScalar, 42, 1, "residual s42"},
        {TableScalar, 48, 1, "residual s48"},
        {TableVector, 1, 4, "residual v4s1"},
        {TableVector, 32, 3, "residual v3s32"},
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
      std::vector<RuleTuple> Tuples;
      for (const Record *Rule : RS->getValueAsListOfDefs("Rules"))
        collectRuleTuples(*Rule, Tuples);
      for (const RuleTuple &Tuple : Tuples)
        Arity = std::max(Arity, unsigned(Tuple.Types.size()));

      const bool HasMem = needsMemDesc(*RS);
      TableType Ptr{TablePointer, 0, 1, "p0"};
      for (StringRef Opcode : RS->getValueAsListOfStrings("Opcodes")) {
        std::string Op = Opcode.str();
        if (!Seen.count(Op))
          Opcodes.push_back(Op);
        auto &List = Points[Op];
        auto &SeenSet = Seen[Op];
        auto Add = [&](const RuleTuple &Tuple) {
          if (Tuple.Types.empty() || Tuple.Types.size() > 3)
            return;
          std::string Key;
          for (const TableType &T : Tuple.Types)
            Key += render(T) + "|";
          if (Tuple.HasMem)
            Key += render(Tuple.MemTy) + "@" + std::to_string(Tuple.Align);
          if (!SeenSet.insert(Key).second)
            return;
          std::string S = "{";
          for (size_t I = 0; I != 3; ++I) {
            if (I)
              S += ", ";
            S += I < Tuple.Types.size() ? render(Tuple.Types[I]) : "LLT()";
          }
          S += "}";
          std::string Mem = Tuple.HasMem
                                ? render(Tuple.MemTy) + ", " +
                                      std::to_string(Tuple.Align) + ", true"
                                : "LLT(), 0, false";
          List.push_back(
              {S, static_cast<unsigned>(Tuple.Types.size()), std::move(Mem)});
        };

        // Every opcode gets a pointer probe with the arity of its guards: the
        // pointer class is the one class whose width is not in the .td.  A
        // memdesc rule set needs a descriptor with the probe, or the chain
        // would read MMODescr[0] of a query that has none.
        RuleTuple Probe;
        Probe.Types.assign(Arity, Ptr);
        Probe.HasMem = HasMem;
        if (HasMem) {
          Probe.MemTy = Ptr;
          Probe.Align = 1;
        }
        if (!TableTuplesOnly)
          Add(Probe);
        for (const RuleTuple &Tuple : Tuples)
          if (Tuple.Types.size() == Arity)
            Add(Tuple);
        if (TableTuplesOnly)
          continue;

        // Types the class list does not name take the reader's residual path:
        // the evaluator is called with the concrete query.  These are the
        // widths and shapes real IR produces that the lattice stops at, so the
        // differential covers the fallback instead of assuming it.
        for (const TableType &Residual : ResidualTypes) {
          RuleTuple T = Probe;
          T.Types[0] = Residual;
          Add(T);
        }
        if (HasMem) {
          // A memory type outside the class list has to reach the evaluator
          // too; the type slots stay in the lattice so only the descriptor
          // axis is exercised.
          RuleTuple T = Probe;
          T.MemTy = {TableVector, 32, 3, "residual v3s32"};
          Add(T);
        }
      }
    }

    unsigned Total = 0;
    for (StringRef Op : Opcodes)
      Total += Points[Op.str()].size();

    std::string P = TablePrefix;
    std::string Lower = P;
    if (!Lower.empty())
      Lower[0] = toLower(Lower[0]);
    OS << "//===-- " << P
       << "LegalizerTablePoints.inc - generated domain "
          "-----*- C++ -*-===//\n";
    OS << "//\n// Generated by llvm-tblgen -gen-legalizer-table-points.  Do "
          "not "
          "edit.\n";
    OS << "//\n//"
          "===-----------------------------------------------------------------"
          "-----===//\n\n";
    OS << "static constexpr unsigned " << P << "NumTablePoints = " << Total
       << ";\n\n";
    OS << "static void " << Lower << "CollectTablePoints(SmallVectorImpl<" << P
       << "TablePoint> &Out, unsigned PointerBits) {\n";
    for (StringRef Op : Opcodes)
      for (const DomainPoint &P : Points[Op.str()])
        OS << "  Out.push_back({TargetOpcode::" << Op << ", \"" << Op << "\", "
           << P.NumTypes << ", " << P.Text << ", " << P.Mem << "});\n";
    OS << "}\n";
  }
};

} // namespace

class LegalizerTableEmitter {
  const RecordKeeper &Records;

public:
  explicit LegalizerTableEmitter(const RecordKeeper &RK) : Records(RK) {}
  void run(raw_ostream &OS) { LegalizerTableEmitterImpl(Records).run(OS); }
};

static TableGen::Emitter::OptClass<LegalizerTableEmitter>
    XTable("gen-legalizer-table", "Emit the GlobalISel legalizer table data");

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

/// The rule index is a test aid, not a reader: -gen-legalizer-table is what a
/// target builds.  The file this emits is included by a unit test only.
class LegalizerRuleIndexEmitter {
  const RecordKeeper &Records;

public:
  explicit LegalizerRuleIndexEmitter(const RecordKeeper &RK) : Records(RK) {}
  void run(raw_ostream &OS) {
    LegalizerTableEmitterImpl(Records).runRuleIndex(OS);
  }
};

static TableGen::Emitter::OptClass<LegalizerRuleIndexEmitter>
    XTableRuleIndex("gen-legalizer-rule-index",
                    "Emit the GlobalISel legalizer rule index (tests only)");

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

/// Report what the lowering registry covers.  The check itself runs in every
/// backend; this one exists so a test can assert the counts without parsing a
/// generated .inc.
class LegalizerCheckEmitter {
  const RecordKeeper &Records;

public:
  explicit LegalizerCheckEmitter(const RecordKeeper &RK) : Records(RK) {}
  void run(raw_ostream &OS) {
    LoweringStats S = verifyLoweringRegistry(Records);
    OS << "{\n"
       << "  \"builders\": " << S.Builders << ",\n"
       << "  \"overloads\": " << S.Overloads << ",\n"
       << "  \"entries\": " << S.Entries << ",\n"
       << "  \"rule_sets\": " << S.RuleSets << ",\n"
       << "  \"rules\": " << S.Rules << ",\n"
       << "  \"escapes\": " << S.Escapes << ",\n"
       << "  \"entries_without_table_translation\": " << S.EntriesWithoutTable
       << ",\n"
       << "  \"rule_sets_without_table_translation\": "
       << S.RuleSetsWithoutTable << ",\n"
       << "  \"core_rules\": " << S.CoreRules << ",\n"
       << "  \"core_operators\": [";
    bool First = true;
    for (const Record *Op :
         Records.getAllDerivedDefinitions("LegalizeCoreOperator")) {
      if (!First)
        OS << ", ";
      First = false;
      OS << "\"" << coreTag(*Op) << "\"";
    }
    OS << "]\n}\n";
  }
};

static TableGen::Emitter::OptClass<LegalizerCheckEmitter>
    XCheck("gen-legalizer-check",
           "Verify the GlobalISel legalizer lowering registry");

