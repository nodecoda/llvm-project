// RUN: llvm-tblgen -gen-legalizer -I %p/../../include %s | FileCheck %s

// Check that a rule set lowers to the same builder chain a target would
// otherwise write by hand, and that predicate ASTs compose.

include "llvm/Target/LegalizerInfo.td"

def Test_CTPOP : LegalizeRuleSet<["G_CTPOP"]> {
  let Rules = [
    LegalFor<LegalizeFeature<"HasPOPCNT">, ["s16, s16", "s32, s32"]>,
    LegalFor<LegalizeAnd<LegalizeFeature<"HasPOPCNT">, LegalizeFeature<"Is64Bit">>,
                ["s64, s64"]>,
    LegalFor<LegalizeOr<LegalizeFeature<"HasVPOPCNTDQ">, LegalizeNot<LegalizeFeature<"NoVLX">>>,
                ["v4s32"], 0>,
    WidenScalarToNextPow2<"1", "16">,
    ClampScalar<"1", "s16", "sMaxScalar">,
    ScalarSameSizeAs<"0", "1">,
  ];
}

// CHECK:      // G_CTPOP
// CHECK-NEXT: getActionDefinitionsBuilder(G_CTPOP)
// CHECK-NEXT: .legalFor(HasPOPCNT, {{[{][{]s16, s16[}], [{]s32, s32[}][}]}})
// CHECK-NEXT: .legalFor((HasPOPCNT && Is64Bit), {{[{][{]s64, s64[}][}]}})
// CHECK-NEXT: .legalFor((HasVPOPCNTDQ || !(NoVLX)), {{[{]v4s32[}]}})
// CHECK-NEXT: .widenScalarToNextPow2(1, /*Min=*/16)
// CHECK-NEXT: .clampScalar(1, s16, sMaxScalar)
// CHECK-NEXT: .scalarSameSizeAs(0, 1);