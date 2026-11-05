// RUN: llvm-tblgen -gen-legalizer-check -I %p/../../include %s | FileCheck %s --check-prefix=CHECK
// RUN: llvm-tblgen -gen-legalizer -I %p/../../include %s | FileCheck %s --check-prefix=CXX
// RUN: llvm-tblgen -gen-legalizer-core -I %p/../../include %s | FileCheck %s --check-prefix=CORE
// RUN: llvm-tblgen -gen-legalizer-table -legalizer-table-prefix=Terminal -I %p/../../include %s | FileCheck %s --check-prefix=TABLE

// A terminal action is the answer itself, so its rule is the operator plus a
// guard and nothing else: no observation, no target.  The four actions that
// name the result carry no type at all; `Scalarize` observes the slot it drops
// the lanes of but still writes no target.

include "llvm/Target/LegalizerInfo.td"

def p0 : LegalizeNamedType<"LLT::pointer(0, 64)">;
def s32 : LegalizeNamedType<"LLT::scalar(32)">;
def s64 : LegalizeNamedType<"LLT::scalar(64)">;

def Is32 : LegalizeFeature<"Subtarget.is32()">;

def TypeSet : LegalizeCoreTypeSet<[s32, s64]>;

def Terminals : LegalizeRuleSet<["G_TERM"]> {
  let Rules = [
    CoreLegal<(CoreIn (Slot 0), TypeSet)>,
    CoreCustom<(CoreAll Is32, (CoreEq (BitsOf (Slot 0)), 32))>,
    CoreLower<(CoreEq (BitsOf (Slot 1)), 64)>,
    CoreUnsupported<(CoreNot Is32)>
  ];
}

def ConditionalScalarize : LegalizeRuleSet<["G_SCALARIZE"]> {
  let Rules = [
    CoreScalarize<(Slot 0)>,
    CoreScalarizeIf<(Slot 0), (CoreEq (BitsOf (Slot 0)), 64)>
  ];
}

// The unconditional forms are the predicate-free chain methods, so they are
// spelled with an always-matching action rather than an always-true lambda.
def Unconditional : LegalizeRuleSet<["G_ALWAYS"]> {
  let Rules = [
    CoreAlwaysLegal,
    CoreAlwaysCustom,
    CoreAlwaysLower,
    CoreAlwaysUnsupported
  ];
}

// CHECK:       "core_rules": 10,
// CHECK:       "core_operators": ["bitcast", "custom", "fewer", "legal", "libcall", "lower", "more", "narrow", "scalarize", "unsupported", "widen"]

// CXX:       .coreLegalIf([=](const LegalityQuery &Query) { return (Query.Types[0] == s32 || Query.Types[0] == s64); })
// CXX:       .coreCustomIf([=](const LegalityQuery &Query) { return (Is32) && (((Query.Types[0]).getScalarSizeInBits() == 32)); })
// CXX:       .coreLowerIf([=](const LegalityQuery &Query) { return ((Query.Types[1]).getScalarSizeInBits() == 64); })
// CXX:       .coreUnsupportedIf([=](const LegalityQuery &Query) { return !(Is32); })
// CXX:       .scalarize(0)
// CXX:       .coreScalarizeIf([=](const LegalityQuery &Query) { return ((Query.Types[0]).getScalarSizeInBits() == 64); }, 0)
// CXX:       .alwaysLegal()
// CXX:       .custom()
// CXX:       .lower()
// CXX:       .unsupported()

// CORE:       "op": "legal", "ruleset": "Terminals"
// CORE:       "action":"Custom", "type_idx":0, "predicate":{"kind":"all"
// CORE:       "op": "custom", "ruleset": "Terminals"
// CORE:       "op": "lower", "ruleset": "Terminals"
// CORE:       "op": "unsupported", "ruleset": "Terminals"
// CORE:       "op": "scalarize", "on": {"kind":"slot","index":0}, "ruleset": "ConditionalScalarize"
// CORE:       "op": "legal", "ruleset": "Unconditional"
// CORE:       "op": "custom", "ruleset": "Unconditional"
// CORE:       "op": "lower", "ruleset": "Unconditional"
// CORE:       "op": "unsupported", "ruleset": "Unconditional"

// The table evaluator carries the action and nothing else: no type index and
// no mutation.  Scalarize is the exception, because the action is a mutation:
// keep the element type and drop the lanes.
// TABLE:       if ((Q.Types[0] == s32 || Q.Types[0] == s64)) {
// TABLE-NEXT:    return {LegalizeActions::Legal, 0, LLT{}};
// TABLE:       if ((Is32) && (((Q.Types[0]).getScalarSizeInBits() == 32))) {
// TABLE-NEXT:    return {LegalizeActions::Custom, 0, LLT{}};
// TABLE:       if (((Q.Types[1]).getScalarSizeInBits() == 64)) {
// TABLE-NEXT:    return {LegalizeActions::Lower, 0, LLT{}};
// TABLE:       if (!(Is32)) {
// TABLE-NEXT:    return {LegalizeActions::Unsupported, 0, LLT{}};
// TABLE:       if (isVector(0)(Q)) {
// TABLE-NEXT:    auto M = (LegalizeMutations::scalarize(0))(Q);
// TABLE-NEXT:    return {LegalizeActions::FewerElements, M.first, M.second};
// TABLE:       if ((((Q.Types[0]).getScalarSizeInBits() == 64)) && (isVector(0)(Q))) {
// TABLE-NEXT:    auto M = (LegalizeMutations::scalarize(0))(Q);
// TABLE-NEXT:    return {LegalizeActions::FewerElements, M.first, M.second};
// The unconditional rules emit their step with no test at all.
// TABLE:       return {LegalizeActions::Legal, 0, LLT{}};
// TABLE-NEXT:  return {LegalizeActions::Custom, 0, LLT{}};
// TABLE-NEXT:  return {LegalizeActions::Lower, 0, LLT{}};
// TABLE-NEXT:  return {LegalizeActions::Unsupported, 0, LLT{}};
