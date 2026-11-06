// RUN: not llvm-tblgen -gen-legalizer -I %p/../../include %s 2>&1 | FileCheck %s

// A core node is identified by its Name string; the checker and both
// emitters dispatch on it.  Two records sharing one name would make every
// lookup silently pick the first, leaving the other unreachable.

include "llvm/Target/LegalizerInfo.td"

def s32 : LegalizeNamedType<"LLT::scalar(32)">;

// "slot" already names Slot.
def SlotAgain : LegalizeCoreExpr<"slot", [SortInt], SortType>;

def R : LegalizeRuleSet<["G_ADD"]> {
  let Rules = [ CoreLegal<(CoreTrue)> ];
}

// CHECK: error: core tag 'slot' is already used by 'Slot'
