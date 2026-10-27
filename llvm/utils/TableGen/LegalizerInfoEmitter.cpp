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
/// is what LGExpr exists for.  Precedence is irrelevant here: the only question
/// is whether the text could have been written with LGAnd / LGOr / LGNot.
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

/// A compound predicate renders as "(...)", which is redundant when the text is
/// a whole function argument - and the hand-written chains do not have it. Drop
/// one pair so a generated fragment can be diffed against the code it replaced.
static StringRef stripRedundantParens(StringRef S) {
  if (S.size() < 2 || S.front() != '(' || S.back() != ')')
    return S;
  unsigned Depth = 0;
  for (size_t I = 0; I != S.size(); ++I) {
    if (S[I] == '(') {
      ++Depth;
    } else if (S[I] == ')' && --Depth == 0 && I + 1 != S.size()) {
      // The leading '(' closes before the end, so it does not wrap the whole
      // argument; leave the text alone.
      return S;
    }
  }
  return S.drop_front().drop_back();
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

  void emitContext(raw_ostream &OS, const Record &Context);
  void verifyRule(const Record &Rule);
  void emitRuleSet(raw_ostream &OS, const Record &RuleSet);

public:
  explicit LegalizerInfoEmitter(const RecordKeeper &RK) : Records(RK) {}
  void run(raw_ostream &OS);
};

} // namespace

void LegalizerInfoEmitter::emitContext(raw_ostream &OS, const Record &Context) {
  for (StringRef Comment : Context.getValueAsListOfStrings("Comments"))
    OS << "  // " << Comment << "\n";

  std::vector<const Record *> Bools = Context.getValueAsListOfDefs("Bools");
  for (const Record *B : Bools)
    OS << "  bool " << B->getValueAsString("Name") << " = "
       << B->getValueAsString("Expr") << ";\n";
  if (!Bools.empty())
    OS << "\n";

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
      std::vector<StringRef> Args = Rule.getValueAsListOfStrings("Args");
      StringRef Sep;
      for (StringRef Arg : Args) {
        OS << Sep << stripRedundantParens(Arg);
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

  // A predicate naming a feature the context does not declare is a typo.  The
  // C++ compiler does catch it, but the diagnostic points into the generated
  // file; this check points at the .td line instead.  Do it before emitting
  // anything so a bad .td file produces no partial output.
  if (!Contexts.empty()) {
    StringSet<> Declared;
    for (const Record *Context : Contexts)
      for (const Record *B : Context->getValueAsListOfDefs("Bools"))
        Declared.insert(B->getValueAsString("Name"));

    for (const Record *F : Records.getAllDerivedDefinitions("LGFeature"))
      if (!Declared.contains(F->getValueAsString("Name")))
        PrintFatalError(F->getLoc(), "feature '" + F->getValueAsString("Name") +
                                         "' is not declared by a "
                                         "LegalizeContext");

    // An LGExpr spelled with nothing but declared features and boolean
    // operators could have been written with the predicate AST.  Letting it
    // through would leave text that looks like a predicate but is opaque, which
    // is the situation the AST is meant to end.
    for (const Record *E : Records.getAllDerivedDefinitions("LGExpr")) {
      SmallVector<StringRef, 4> Idents;
      StringRef Expr = E->getValueAsString("Expr");
      if (!isBooleanCombination(Expr, Idents))
        continue;
      if (!all_of(Idents, [&](StringRef Id) {
            return Id == "true" || Id == "false" || Declared.contains(Id);
          }))
        continue;
      std::string Hint;
      raw_string_ostream HintOS(Hint);
      HintOS << "LGExpr '" << Expr
             << "' is a boolean combination of declared features; write it "
                "with LGAnd / LGOr / LGNot";
      PrintFatalError(E->getLoc(), HintOS.str());
    }
  }

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
