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
};

} // namespace

/// Collapse whitespace runs, including newlines, into single spaces and trim.
/// A condition may be written across several lines in the .td; the generated
/// code is one statement per feature.
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
    OS << "  bool " << F->getName() << " = "
       << squashSpace(F->getValueAsString("Cond")) << ";\n";
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
  auto BySourceOrder = [](const Record *L, const Record *R) {
    return L->getID() < R->getID();
  };

  std::vector<const Record *> Types =
      Records.getAllDerivedDefinitions("LegalizeType");
  llvm::sort(Types, BySourceOrder);
  for (const Record *T : Types)
    OS << "  const LLT " << T->getName() << " = " << T->getValueAsString("Expr")
       << ";\n";
  if (!Types.empty())
    OS << "\n";

  std::vector<const Record *> Lists =
      Records.getAllDerivedDefinitions("LegalizeTypeList");
  llvm::sort(Lists, BySourceOrder);
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

void LegalizerInfoEmitter::verifyRule(const Record &Rule) {
  StringRef Method = Rule.getValueAsString("Method");
  if (Method.empty())
    PrintFatalError(Rule.getLoc(), "legalizer rule is missing its Method");

  const Record *Action = Rule.getValueAsOptionalDef("Action");
  if (!Action)
    PrintFatalError(Rule.getLoc(),
                    "legalizer rule '" + Method + "' does not name an Action");
  if (!Action->isSubClassOf("LegalizeAction"))
    PrintFatalError(Rule.getLoc(),
                    "'" + Action->getName() + "' is not a LegalizeAction");

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

  // A type is rendered by its record name, so it has to be a named def: an
  // inline `LegalizeType<...>` would generate an identifier called anonymous_0.
  auto CheckNamed = [](const Record *T) {
    if (T->isSubClassOf("LegalizeType") && T->isAnonymous())
      PrintFatalError(T, "a legalizer type has to be a named def, e.g. "
                         "`def s32 : LegalizeType<...>`");
  };
  for (const Record *Arg : Rule.getValueAsListOfDefs("Args")) {
    CheckNamed(Arg);
    if (Arg->isSubClassOf("LegalizeTypePair")) {
      CheckNamed(Arg->getValueAsDef("Type0"));
      CheckNamed(Arg->getValueAsDef("Type1"));
    } else if (Arg->isSubClassOf("LegalizeMemDesc")) {
      CheckNamed(Arg->getValueAsDef("Type0"));
      CheckNamed(Arg->getValueAsDef("Type1"));
      CheckNamed(Arg->getValueAsDef("MemTy"));
    }
  }
}

/// Render one argument.  Shapes that stand for a group of types brace
/// themselves, so a list of pairs reads {{a, b}, {c, d}}; the brace around the
/// whole list is added by emitArgList.
void LegalizerInfoEmitter::emitArg(const Record &Arg, raw_ostream &OS) {
  if (Arg.isSubClassOf("LegalizeType") ||
      Arg.isSubClassOf("LegalizeTypeList")) {
    OS << Arg.getName();
    return;
  }
  if (Arg.isSubClassOf("LegalizeTypeExpr")) {
    OS << Arg.getValueAsString("Text");
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

/// A list made only of types, type pairs or memdesc tuples is one braced group
/// in C++ -- legalFor({s8, s16}), legalFor({{s32, s32}, ...}),
/// legalForTypesWithMemDesc({{s8, p0, s8, 1}, ...}).  Anything else is a plain
/// argument list in which each argument renders itself, which is what the
/// builders that take an index and two types need.
static bool isBracedGroup(ArrayRef<const Record *> Args) {
  if (Args.empty())
    return false;
  return all_of(Args, [](const Record *A) {
    return A->isSubClassOf("LegalizeType") ||
           A->isSubClassOf("LegalizeTypeExpr") ||
           A->isSubClassOf("LegalizeTypePair") ||
           A->isSubClassOf("LegalizeMemDesc");
  });
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
    OS << "      ." << Rule.getValueAsString("Method") << "(";
    if (Opaque.empty()) {
      if (const Record *Pred = Rule.getValueAsOptionalDef("Pred")) {
        emitPredicate(*Pred, OS);
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

  emitFeatures(OS);
  emitTypes(OS);

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
