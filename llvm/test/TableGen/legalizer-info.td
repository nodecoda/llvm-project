// RUN: llvm-tblgen -gen-legalizer -I %p/../../include %s | FileCheck %s

// Check that a rule set lowers to the same builder chain a target would
// otherwise write by hand, and that predicate ASTs compose.

include "llvm/Target/LegalizerInfo.td"

// A leaf is a record whose name is the C++ identifier; -gen-legalizer emits the
// declaration ahead of the rules, in .td order.
def Is64Bit : LegalizeFeature<"Subtarget.is64Bit()">;
def HasPOPCNT : LegalizeFeature<"Subtarget.hasPOPCNT()">;
def HasVPOPCNTDQ : LegalizeFeature<"Subtarget.hasVPOPCNTDQ()">;
def NoVLX : LegalizeFeature<"!Subtarget.hasVLX()">;

// Combinations are named records: a dag argument cannot hold a bare record.
def PopcntAndIs64Bit : LegalizePredicateDag<(all_of HasPOPCNT, Is64Bit)>;
def VpopcntdqOrNotNoVLX
    : LegalizePredicateDag<(any_of HasVPOPCNTDQ, (not NoVLX))>;
def NeitherVpopcntdqNorVLX
    : LegalizePredicateDag<(none_of HasVPOPCNTDQ, NoVLX)>;

def s16 : LegalizeType<"LLT::scalar(16)">;
def s32 : LegalizeType<"LLT::scalar(32)">;
def s64 : LegalizeType<"LLT::scalar(64)">;
def sMaxScalar : LegalizeType<"Subtarget.is64Bit() ? s64 : s32">;
def v4s32 : LegalizeType<"LLT::fixed_vector(4, 32)">;
def Tys : LegalizeTypeList<[s32]>;

// CHECK:      bool Is64Bit = Subtarget.is64Bit();
// CHECK:      const LLT s32 = LLT::scalar(32);
// CHECK:      const std::initializer_list<LLT> Tys = {s32};

def Test_CTPOP : LegalizeRuleSet<["G_CTPOP"]> {
  let Rules = [
    LegalForPairsIf<HasPOPCNT, [LegalizeTypePair<s16, s16>,
                                LegalizeTypePair<s32, s32>]>,
    LegalForPairsIf<PopcntAndIs64Bit, [LegalizeTypePair<s64, s64>]>,
    LegalForIf<VpopcntdqOrNotNoVLX, [v4s32]>,
    WidenScalarToNextPow2<0>,
    WidenScalarToNextPow2<1, 16>,
    ClampScalar<1, s16, sMaxScalar>,
    ScalarSameSizeAs<0, 1>,
  ];
}

// CHECK:      getActionDefinitionsBuilder(G_CTPOP)
// CHECK-NEXT: .legalFor(HasPOPCNT, {{[{][{]s16, s16[}], [{]s32, s32[}][}]}})
// CHECK-NEXT: .legalFor(HasPOPCNT && Is64Bit, {{[{][{]s64, s64[}][}]}})
// CHECK-NEXT: .legalFor(HasVPOPCNTDQ || !(NoVLX), {{[{]v4s32[}]}})
// CHECK-NEXT: .widenScalarToNextPow2(0)
// CHECK-NEXT: .widenScalarToNextPow2(1, /*Min=*/16)
// CHECK-NEXT: .clampScalar(1, s16, sMaxScalar)
// CHECK-NEXT: .scalarSameSizeAs(0, 1);

// Rule sets are emitted in .td order rather than sorted by name, so that a
// generated fragment lines up with the file it was written in.
def Test_ADD : LegalizeRuleSet<["G_ADD"]> {
  let Rules = [
    LegalForIf<NeitherVpopcntdqNorVLX, [s32]>,
    // A call is not a boolean combination, so this one keeps the escape
    // hatch: it is for guards that have no AST form yet.
    ScalarizeIf<LegalizeOpaquePred<"scalarOrEltWiderThan(0, 64)">, 0>,
    Lower,
  ];
}

// CHECK:      getActionDefinitionsBuilder(G_ADD)
// CHECK-NEXT: .legalFor(!(HasVPOPCNTDQ || NoVLX), {{[{]s32[}]}})
// CHECK-NEXT: .scalarizeIf(scalarOrEltWiderThan(0, 64), 0)
// CHECK-NEXT: .lower();

// A cartesian product takes two type lists.  The braces around an inline list
// belong to the emitter; a list picked by an expression cannot carry them,
// because the choice is made at run time.
def Test_CARTESIAN : LegalizeRuleSet<["G_PTRTOINT"]> {
  let Rules = [
    LegalForCartesianProduct<LegalizeInlineTypeList<[s16, s32]>,
                             LegalizeTypeListExpr<"Is64Bit ? Tys : Tys">>,
  ];
}

// CHECK:      getActionDefinitionsBuilder(G_PTRTOINT)
// CHECK-NEXT: .legalForCartesianProduct({s16, s32}, Is64Bit ? Tys : Tys);
