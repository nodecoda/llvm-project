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

// A condition may name another feature, and that reference keeps the named one
// reachable.  SlowMem is named here and nowhere else: if -gen-legalizer did not
// follow the reference it would report SlowMem as unused, not declare it, and
// the generated SlowMemOr64 would not compile.
def SlowMem : LegalizeFeature<"Subtarget.hasSlowUAMem()">;
def SlowMemOr64 : LegalizeFeature<"SlowMem || Is64Bit">;

// Combinations are named records: a dag argument cannot hold a bare record.
def PopcntAndIs64Bit : LegalizePredicateDag<(all_of HasPOPCNT, Is64Bit)>;
def VpopcntdqOrNotNoVLX
    : LegalizePredicateDag<(any_of HasVPOPCNTDQ, (not NoVLX))>;
def NeitherVpopcntdqNorVLX
    : LegalizePredicateDag<(none_of HasVPOPCNTDQ, NoVLX)>;

def s16 : LegalizeNamedType<"LLT::scalar(16)">;
def s32 : LegalizeNamedType<"LLT::scalar(32)">;
def s64 : LegalizeNamedType<"LLT::scalar(64)">;
def sMaxScalar : LegalizeNamedType<"Subtarget.is64Bit() ? s64 : s32">;
def v4s32 : LegalizeNamedType<"LLT::fixed_vector(4, 32)">;
def Tys : LegalizeNamedTypeList<[s32]>;

// CHECK:      bool SlowMem = Subtarget.hasSlowUAMem();
// CHECK:      bool SlowMemOr64 = SlowMem || Is64Bit;
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
    ScalarizeIf<LegalizeQueryPredicate<"scalarOrEltWiderThan(0, 64)">, 0>,
    Lower,
  ];
}

// CHECK:      getActionDefinitionsBuilder(G_ADD)
// CHECK-NEXT: .legalFor(!(HasVPOPCNTDQ || NoVLX), {{[{]s32[}]}})
// CHECK-NEXT: .scalarizeIf(scalarOrEltWiderThan(0, 64), 0)
// CHECK-NEXT: .lower();

// A cartesian product takes two type lists.  The braces around an inline list
// belong to the emitter; a named list is passed by name, and an expression that
// picks one of two named lists cannot carry braces because the choice is made
// at run time.
def Test_CARTESIAN : LegalizeRuleSet<["G_PTRTOINT"]> {
  let Rules = [
    LegalForCartesianProduct<LegalizeInlineTypeList<[s16, s32]>, Tys>,
    LegalForCartesianProduct<LegalizeTypeListChoice<"Is64Bit ? Tys : Tys">, Tys>,
  ];
}

// CHECK:      getActionDefinitionsBuilder(G_PTRTOINT)
// CHECK-NEXT: .legalForCartesianProduct({s16, s32}, Tys)
// CHECK-NEXT: .legalForCartesianProduct(Is64Bit ? Tys : Tys, Tys);

// The only reference to SlowMem is the condition of SlowMemOr64, so this rule
// set is what keeps both of them in the generated constructor.
def Test_FENCE : LegalizeRuleSet<["G_FENCE"]> {
  let Rules = [LegalForIf<SlowMemOr64, [s32]>];
}

// CHECK:      getActionDefinitionsBuilder(G_FENCE)
// CHECK-NEXT: .legalFor(SlowMemOr64, {{[{]s32[}]}});