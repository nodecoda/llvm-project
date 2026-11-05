// RUN: llvm-tblgen -gen-legalizer -I %p/../../include %s | FileCheck %s

// A feature whose only reference is a select must still be declared.  Both
// selects are inline instantiations, so a collector that only walks named
// predicate records misses them and the generated fragment names an
// undeclared identifier.

include "llvm/Target/LegalizerInfo.td"

def s16 : LegalizeNamedType<"LLT::scalar(16)">;
def s32 : LegalizeNamedType<"LLT::scalar(32)">;

def OnlyInCoreSelect   : LegalizeFeature<"Subtarget.coreOnly()">;
def OnlyInLegacySelect : LegalizeFeature<"Subtarget.legacyOnly()">;

def R : LegalizeRuleSet<["G_TEST"]> {
  let Rules = [
    WidenScalarOrEltToNextPow2OrMinSizeSel<0, LegalizeSelectBits<[OnlyInLegacySelect], [16], 32>>,
    CoreWiden<(Slot 0), (CoreSelect OnlyInCoreSelect, 16, 32)>
  ];
}

// CHECK: bool OnlyInCoreSelect = Subtarget.coreOnly();
// CHECK: bool OnlyInLegacySelect = Subtarget.legacyOnly();
// CHECK: .widenScalarOrEltToNextPow2OrMinSize(0, (OnlyInLegacySelect ? 16 : 32))
// CHECK: .coreWidenScalarOrEltToNextPow2OrMinSize({{.*}}, 0, (OnlyInCoreSelect ? 16 : 32))
