// RUN: llvm-tblgen -gen-legalizer -I %p/../../include %s | FileCheck %s

// Check that a rule set lowers to the same builder chain a target would
// otherwise write by hand, and that predicate ASTs compose.

include "llvm/Target/LegalizerInfo.td"

def Test_CTPOP : LegalizeRuleSet<["G_CTPOP"]> {
  let Rules = [
    LG_LegalFor<LGFeature<"HasPOPCNT">, ["s16, s16", "s32, s32"]>,
    LG_LegalFor<LGAnd<LGFeature<"HasPOPCNT">, LGFeature<"Is64Bit">>,
                ["s64, s64"]>,
    LG_LegalFor<LGOr<LGFeature<"HasVPOPCNTDQ">, LGNot<LGFeature<"NoVLX">>>,
                ["v4s32"], 0>,
    LG_WidenScalarToNextPow2<"0">,
    LG_WidenScalarToNextPow2<"1", "16">,
    LG_ClampScalar<"1", "s16", "sMaxScalar">,
    LG_ScalarSameSizeAs<"0", "1">,
  ];
}

// CHECK:      getActionDefinitionsBuilder(G_CTPOP)
// CHECK-NEXT: .legalFor(HasPOPCNT, {{[{][{]s16, s16[}], [{]s32, s32[}][}]}})
// CHECK-NEXT: .legalFor((HasPOPCNT && Is64Bit), {{[{][{]s64, s64[}][}]}})
// CHECK-NEXT: .legalFor((HasVPOPCNTDQ || !(NoVLX)), {{[{]v4s32[}]}})
// CHECK-NEXT: .widenScalarToNextPow2(0)
// CHECK-NEXT: .widenScalarToNextPow2(1, /*Min=*/16)
// CHECK-NEXT: .clampScalar(1, s16, sMaxScalar)
// CHECK-NEXT: .scalarSameSizeAs(0, 1);

// Rule sets are emitted in .td order rather than sorted by name, so that a
// generated fragment lines up with the file it was written in.
def Test_ADD : LegalizeRuleSet<["G_ADD"]> {
  let Rules = [LG_Lower];
}

// CHECK:      getActionDefinitionsBuilder(G_ADD)
// CHECK-NEXT: .lower();