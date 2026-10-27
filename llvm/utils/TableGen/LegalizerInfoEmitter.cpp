//===- LegalizerInfoEmitter.cpp - Emit legalizer rule fragments -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POC: emit GlobalISel legalizer rule chains from TableGen.
//
// The output is a statement sequence (not a data table), meant to be included
// inside a target's LegalizerInfo constructor, e.g.
//
//   getActionDefinitionsBuilder(G_CTPOP)
//       .legalFor(HasPOPCNT, {{s16, s16}, {s32, s32}})
//       .clampScalar(1, s16, sMaxScalar)
//       .scalarSameSizeAs(0, 1);
//
// Rationale and design notes: see the analysis in llvm-dev/ (LegalizerInfo
// TableGen conversion).  The checks below are the point of the exercise: they are
// generation-time checks that the hand-written C++ cannot express, because
// there the predicate is an opaque std::function.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TableGen/Error.h"
#include "llvm/TableGen/Record.h"
#include "llvm/TableGen/TableGenBackend.h"

using namespace llvm;

namespace {

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

  void verifyRule(const Record &Rule);
  void emitRuleSet(raw_ostream &OS, const Record &RuleSet);

public:
  explicit LegalizerInfoEmitter(const RecordKeeper &RK) : Records(RK) {}
  void run(raw_ostream &OS);
};

} // namespace

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

  OS << "  // " << Opcodes.front() << "\n";
  OS << "  getActionDefinitionsBuilder(" << join(Opcodes, ", ") << ")\n";

  for (size_t I = 0, E = Rules.size(); I != E; ++I) {
    const Record &Rule = *Rules[I];
    verifyRule(Rule);
    OS << "      ." << Rule.getValueAsString("Method") << "("
       << join(Rule.getValueAsListOfStrings("Args"), ", ") << ")"
       << (I + 1 == E ? ";\n" : "\n");
  }
}

void LegalizerInfoEmitter::run(raw_ostream &OS) {
  emitSourceFileHeader("GlobalISel legalizer rule fragments", OS);

  std::vector<const Record *> RuleSets =
      Records.getAllDerivedDefinitions("LegalizeRuleSet");
  if (RuleSets.empty())
    PrintFatalError("no LegalizeRuleSet records found");

  // A duplicated opcode silently *appends* rules to the same rule set at
  // runtime, so this cannot be caught by the C++ compiler either.
  StringSet<> SeenOpcodes;

  for (const Record *RuleSet : RuleSets) {
    for (StringRef Opcode : RuleSet->getValueAsListOfStrings("Opcodes")) {
      if (!StringRef(Opcode).starts_with("G_"))
        PrintFatalError(RuleSet->getLoc(),
                        "'" + Opcode + "' is not a generic opcode");
      if (!SeenOpcodes.insert(Opcode).second)
        PrintFatalError(RuleSet->getLoc(),
                        "opcode '" + Opcode +
                            "' is described by more than one rule set");
    }
    emitRuleSet(OS, *RuleSet);
  }
}

static TableGen::Emitter::OptClass<LegalizerInfoEmitter>
    X("gen-legalizer", "Generate GlobalISel legalizer rule fragments");