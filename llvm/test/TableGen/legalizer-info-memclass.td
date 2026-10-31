// RUN: llvm-tblgen -gen-legalizer-table -I %p/../../include %s | FileCheck %s

// A rule that compares the whole memory type cannot be keyed by access size:
// two memory types of the same size are still different types.  When the rule
// set names memory types, the axis is cut on those classes instead of on
// sizes, and the table says so with MemIsClass.

include "llvm/Target/LegalizerInfo.td"

def s64 : LegalizeNamedType<"LLT::scalar(64)">;
def v2s32 : LegalizeNamedType<"LLT::fixed_vector(2, 32)">;
def p0 : LegalizeNamedType<"LLT::pointer(0, 64)">;

// The pair that a size key would merge: both are 64 bits.
def DiffMem : LegalizeTypePredicate<"typeDiffersFromMemDesc(0, 0)">;

def Test_STORE : LegalizeRuleSet<["G_STORE"]> {
  let Rules = [
    LegalForMemDesc<[LegalizeMemDesc<s64, p0, s64, 1>,
                     LegalizeMemDesc<v2s32, p0, v2s32, 1>]>,
    CustomIf<DiffMem>,
  ];
}

// Two 64-bit memory types, kept apart by the class key.
// CHECK-NOT: Not tabulated
// CHECK: static constexpr uint8_t TestLocalT0S2[] = {0, 1};
// CHECK: static constexpr uint8_t TestDomainT0S2[] = {1, 2};
// CHECK: static constexpr int32_t TestMemT0[] = {1, 2};
// CHECK: TestMemT0, true,
// CHECK: // Test_STORE
