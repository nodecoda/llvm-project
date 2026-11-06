// RUN: not llvm-tblgen -gen-legalizer -I %p/../../include %s 2>&1 | FileCheck %s

// G_ADD declares one generic type (type0 on all three operands), so its
// LegalityQuery has exactly one type slot.  Writing (Slot 1) used to be
// accepted: arityOf() inferred the arity *from the rules*, so the out-of-range
// index simply made the rule set look like a two-slot one, and the emitted
// predicate read past the end of Query.Types.

// Target.td is included first, as a target would: it is what declares the
// generic opcodes and their operand types.
include "llvm/Target/LegalizerInfo.td"
include "llvm/Target/Target.td"

def T_s32 : LegalizeNamedType<"LLT::scalar(32)">;

def R : LegalizeRuleSet<["G_ADD"]> {
  let Rules = [ CoreWiden<(Slot 1), (Pow2)> ];
}

// CHECK: error: core rule: rule set 'R' reads type slot 1, but G_ADD has 1 type slot(s)
