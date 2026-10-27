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
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TableGen/Error.h"
#include "llvm/TableGen/Record.h"
#include "llvm/TableGen/TableGenBackend.h"

#include <algorithm>

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
/// is what LegalizeOpaquePred exists for.  Precedence is irrelevant here: the
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

  /// The mutation direction an action requires, or "" if the action does not
  /// prescribe one.  "any" means the rule is allowed to choose.
  static StringRef requiredMutationDirection(StringRef Action) {
    return StringSwitch<StringRef>(Action)
        .Case("WidenScalar", "widen")
        .Case("NarrowScalar", "narrow")
        .Case("FewerElements", "split")
        .Case("MoreElements", "merge")
        .Case("Bitcast", "bitcast")
        .Default("");
  }

  void emitComments(raw_ostream &OS, const Record &Context);
  void emitFeatures(raw_ostream &OS);
  void emitPredicate(const Record &P, raw_ostream &OS);
  void emitContext(raw_ostream &OS, const Record &Context);
  void verifyRule(const Record &Rule);
  void emitRuleSet(raw_ostream &OS, const Record &RuleSet);

public:
  explicit LegalizerInfoEmitter(const RecordKeeper &RK) : Records(RK) {}
  void run(raw_ostream &OS);
};

} // namespace

void LegalizerInfoEmitter::emitComments(raw_ostream &OS,
                                        const Record &Context) {
  for (StringRef Comment : Context.getValueAsListOfStrings("Comments"))
    OS << "  // " << Comment << "\n";
}

/// Emit `bool <name> = <expr>;` for every LegalizeFeature record, in .td order.
/// The record name *is* the C++ identifier, which is why a rule can refer to a
/// feature by writing the record itself instead of a string.
void LegalizerInfoEmitter::emitFeatures(raw_ostream &OS) {
  std::vector<const Record *> Features =
      Records.getAllDerivedDefinitions("LegalizeFeature");
  llvm::sort(Features, [](const Record *L, const Record *R) {
    return L->getID() < R->getID();
  });
  for (const Record *F : Features)
    OS << "  bool " << F->getName() << " = " << F->getValueAsString("Cond")
       << ";\n";
  if (!Features.empty())
    OS << "\n";
}

/// Lower one predicate record to a C++ boolean expression.  A combination is
/// walked by the shared emitPredicateDag(); a leaf is either a cached bool
/// (LegalizeFeature) or verbatim text (LegalizeOpaquePred).
void LegalizerInfoEmitter::emitPredicate(const Record &P, raw_ostream &OS) {
  if (P.isSubClassOf("LegalizePredicateDag")) {
    auto EmitLeaf = [&](const Init &Leaf, raw_ostream &OS) -> bool {
      const auto *DI = dyn_cast<DefInit>(&Leaf);
      if (!DI || !DI->getDef()->isSubClassOf("LegalizeFeature"))
        PrintFatalError(&P, "predicate dag leaf '" + Leaf.getAsString() +
                                "' is not a LegalizeFeature");
      OS << DI->getDef()->getName();
      return false;
    };
    emitPredicateDag(&P, *P.getValueAsDag("CondDag"), /*ParenIfBinOp=*/false,
                     OS, EmitLeaf);
    return;
  }
  if (P.isSubClassOf("LegalizeFeature")) {
    OS << P.getName();
    return;
  }
  if (P.isSubClassOf("LegalizeOpaquePred")) {
    OS << P.getValueAsString("Expr");
    return;
  }
  PrintFatalError(&P, "unknown LegalizePredicate record");
}

void LegalizerInfoEmitter::emitContext(raw_ostream &OS, const Record &Context) {
  std::vector<const Record *> Types = Context.getValueAsListOfDefs("Types");
  for (const Record *T : Types)
    OS << "  const LLT " << T->getValueAsString("Name") << " = "
       << T->getValueAsString("Expr") << ";\n";
  if (!Types.empty())
    OS << "\n";

  std::vector<const Record *> Lists = Context.getValueAsListOfDefs("TypeLists");
  for (const Record *L : Lists)
    OS << "  const std::initializer_list<LLT> " << L->getValueAsString("Name")
       << " = {" << join(L->getValueAsListOfStrings("Elems"), ", ") << "};\n";
  if (!Lists.empty())
    OS << "\n";
}

void LegalizerInfoEmitter::verifyRule(const Record &Rule) {
  StringRef Method = Rule.getValueAsString("Method");
  if (Method.empty())
    PrintFatalError(Rule.getLoc(), "legalizer rule is missing its Method");

  StringRef Action = Rule.getValueAsString("Action");
  StringRef Mutation = Rule.getValueAsString("Mutation");

  // This is the check that hand-written C++ cannot do: the action and the
  // mutation live in two different places there, and nothing ties them
  // together (this is exactly the bug class behind
  // https://github.com/llvm/llvm-project/issues/220802).
  StringRef Required = requiredMutationDirection(Action);
  if (!Required.empty() && Mutation != Required && Mutation != "any")
    PrintFatalError(Rule.getLoc(),
                    "action '" + Action + "' requires a '" + Required +
                        "' mutation, but this rule carries '" + Mutation + "'");

  if (Rule.getValueAsString("OpaqueArgs").empty())
    for (StringRef Arg : Rule.getValueAsListOfStrings("Args"))
      if (Arg.empty())
        PrintFatalError(Rule.getLoc(), "legalizer rule '" + Method +
                                           "' has an empty argument");
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
    OS << "      ." << Rule.getValueAsString("Method") << "(";
    if (Opaque.empty()) {
      StringRef Sep;
      if (const Record *Pred = Rule.getValueAsOptionalDef("Pred")) {
        emitPredicate(*Pred, OS);
        Sep = ", ";
      }
      for (StringRef Arg : Rule.getValueAsListOfStrings("Args")) {
        OS << Sep << Arg;
        Sep = ", ";
      }
    } else
      emitOpaqueBody(OS, Opaque);
    OS << (I + 1 == E ? ");\n" : ")\n");
  }
}

void LegalizerInfoEmitter::run(raw_ostream &OS) {
  emitSourceFileHeader("GlobalISel legalizer rule fragments", OS);

  std::vector<const Record *> Contexts =
      Records.getAllDerivedDefinitions("LegalizeContext");
  llvm::sort(Contexts, [](const Record *L, const Record *R) {
    return L->getID() < R->getID();
  });

  // A feature leaf that names something the .td never defined is rejected by
  // TableGen itself ("Variable not defined"), which points at the .td line and
  // runs before this backend.  What is left to check is the escape hatch: an
  // opaque predicate spelled with nothing but declared features and boolean
  // operators could have been written with the dag form, and letting it
  // through would leave text that looks like a predicate but is opaque.
  StringSet<> Declared;
  for (const Record *F : Records.getAllDerivedDefinitions("LegalizeFeature"))
    Declared.insert(F->getName());

  for (const Record *E :
       Records.getAllDerivedDefinitions("LegalizeOpaquePred")) {
    SmallVector<StringRef, 4> Idents;
    StringRef Expr = E->getValueAsString("Expr");
    if (!isBooleanCombination(Expr, Idents))
      continue;
    if (!all_of(Idents, [&](StringRef Id) {
          return Id == "true" || Id == "false" || Declared.contains(Id);
        }))
      continue;
    PrintFatalError(E->getLoc(),
                    "LegalizeOpaquePred '" + Expr +
                        "' is a boolean combination of declared features; "
                        "write it with all_of / any_of / not");
  }

  for (const Record *Context : Contexts)
    emitComments(OS, *Context);
  emitFeatures(OS);
  for (const Record *Context : Contexts)
    emitContext(OS, *Context);

  std::vector<const Record *> RuleSets =
      Records.getAllDerivedDefinitions("LegalizeRuleSet");
  if (RuleSets.empty())
    PrintFatalError("no LegalizeRuleSet records found");

  // getAllDerivedDefinitions() returns the records sorted by name.  Emit them
  // in source order instead, so that a generated fragment lines up with the
  // .td file and, for a mechanical conversion, with the code it replaces.
  llvm::sort(RuleSets, [](const Record *L, const Record *R) {
    return L->getID() < R->getID();
  });

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

static TableGen::Emitter::OptClass<LegalizerInfoEmitter>
    X("gen-legalizer", "Generate GlobalISel legalizer rule fragments");
