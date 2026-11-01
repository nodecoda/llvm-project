// RUN: llvm-tblgen -gen-legalizer-table -I %p/../../include %s | FileCheck %s

// A FLOAT spelling of a class can only be asked of the chain when the rules
// never force an element width that has no FP semantics: LLT::changeElementSize
// on a FLOAT has to name an FP semantics and aborts at any other width.  The
// descriptor says per keyed type axis whether that holds, and the self-check
// only expands the fiber there.  Derived from the mutations, not listed by
// hand: the two rule sets below differ only in the bound they clamp to.

include "llvm/Target/LegalizerInfo.td"

def s8 : LegalizeNamedType<"LLT::scalar(8)">;
def s32 : LegalizeNamedType<"LLT::scalar(32)">;
def s64 : LegalizeNamedType<"LLT::scalar(64)">;

// Widening to the next power of two with a floor of 32 leaves an IEEE width
// alone (16/32/64/128 are powers of two and at or above the floor), so the
// float spelling is safe to ask.
def Test_WIDEN : LegalizeRuleSet<["G_ADD"]> {
  let Rules = [LegalFor<[s32]>, WidenScalarToNextPow2<0, 32>];
}

// Clamping to s8 forces 8 bits on any wider type, and 8 bits has no FP
// semantics: the float spelling is not a question the chain can answer.
def Test_CLAMP : LegalizeRuleSet<["G_SUB"]> {
  let Rules = [LegalFor<[s32]>, ClampScalar<0, s8, s64>];
}

// FloatOK is the field after MemIsClass; 1 means axis 0 may be asked a FLOAT
// spelling, 0 means it may not.
// CHECK: nullptr, false,   1, {{.*}}// Test_WIDEN
// CHECK: nullptr, false,   0, {{.*}}// Test_CLAMP
