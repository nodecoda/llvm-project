// RUN: llvm-tblgen -gen-legalizer-table -legalizer-table-prefix=Replace -I %p/../../include %s | FileCheck %s
// RUN: llvm-tblgen -gen-legalizer-table-points -legalizer-table-prefix=Replace -I %p/../../include %s | FileCheck %s --check-prefix=POINTS

// A `Replace` target is a whole-type assignment, so the table evaluator has
// to build the same mutation the chain does.  Every operator that can assign
// a type is covered, because a shim the table backend cannot translate is a
// build error rather than a silent fallback.

include "llvm/Target/LegalizerInfo.td"

def p0  : LegalizeNamedType<"LLT::pointer(0, 64)">;
def s32 : LegalizeNamedType<"LLT::scalar(32)">;
def s64 : LegalizeNamedType<"LLT::scalar(64)">;

def Assigned : LegalizeRuleSet<["G_ASSIGN"]> {
  let Rules = [
    CoreLegal<(CoreEq (Slot 0), (Ty s32))>,
    CoreWiden<(Slot 0), (Replace (SameAs 1))>,
    CoreNarrow<(Slot 0), (Replace (Ty s32))>,
    CoreMore<(Slot 1), (Replace (DivLanes (Slot 1), 2))>,
    CoreFewer<(Slot 1), (Replace (VecOf 4, (Ty s32)))>,
    CoreBitcast<(Slot 0), (Replace (IntOfSize (SizeOf (Slot 0))))>
  ];
}

// Two unconditional mutating rules in a row: each step declares `auto M`, so
// an unbraced step would redeclare it in the same scope.
def Unconditional : LegalizeRuleSet<["G_UNCOND"]> {
  let Rules = [
    CoreWiden<(Slot 0), (Replace (Ty s64))>,
    CoreNarrow<(Slot 0), (Replace (Ty s32))>
  ];
}

// CHECK:      if ((Q.Types[0] == s32)) {
// CHECK-NEXT:   return {LegalizeActions::Legal, 0, LLT{}};
// CHECK-NEXT: }
// CHECK-NEXT: {
// CHECK-NEXT:   auto M = ([=](const LegalityQuery &Q) { return std::make_pair(unsigned(0), Q.Types[1]); })(Q);
// CHECK-NEXT:   return {LegalizeActions::WidenScalar, M.first, M.second};
// CHECK-NEXT: }
// CHECK-NEXT: {
// CHECK-NEXT:   auto M = ([=](const LegalityQuery &Q) { return std::make_pair(unsigned(0), s32); })(Q);
// CHECK-NEXT:   return {LegalizeActions::NarrowScalar, M.first, M.second};
// CHECK-NEXT: }
// CHECK-NEXT: {
// CHECK-NEXT:   auto M = ([=](const LegalityQuery &Q) { return std::make_pair(unsigned(1), (Q.Types[1]).changeElementCount((Q.Types[1]).getElementCount().divideCoefficientBy(2))); })(Q);
// CHECK-NEXT:   return {LegalizeActions::MoreElements, M.first, M.second};
// CHECK-NEXT: }
// CHECK-NEXT: {
// CHECK-NEXT:   auto M = ([=](const LegalityQuery &Q) { return std::make_pair(unsigned(1), LLT::fixed_vector(4, s32)); })(Q);
// CHECK-NEXT:   return {LegalizeActions::FewerElements, M.first, M.second};
// CHECK-NEXT: }
// CHECK-NEXT: {
// CHECK-NEXT:   auto M = ([=](const LegalityQuery &Q) { return std::make_pair(unsigned(0), LLT::integer((Q.Types[0]).getSizeInBits().getKnownMinValue())); })(Q);
// CHECK-NEXT:   return {LegalizeActions::Bitcast, M.first, M.second};
// CHECK-NEXT: }

// POINTS: replaceCollectTablePoints
// POINTS: G_ASSIGN
