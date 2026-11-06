// RUN: not llvm-tblgen -gen-legalizer-table-points -I %p/../../include %s 2>&1 | FileCheck %s

// The domain probe covers three type slots.  It used to clamp silently, so a
// rule reading Types[3] was probed as a three-slot query: the rule would index
// past the array at run time and the domain would look complete.  There is no
// static upper bound to check against (LegalityQuery::Types is an ArrayRef),
// so the honest check is against the limit this emitter actually has.

include "llvm/Target/LegalizerInfo.td"

def s32 : LegalizeNamedType<"LLT::scalar(32)">;
def p0  : LegalizeNamedType<"LLT::pointer(0, 64)">;

def R : LegalizeRuleSet<["G_ADD"]> {
  let Rules = [ CoreLegal<(CoreIn (Slot 0), [p0])>,
                CoreWiden<(Slot 3), (Pow2)> ];
}

// CHECK: error: rule set 'R' reads type slot 3, but the domain probe covers at most three
