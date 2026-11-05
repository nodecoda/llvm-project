// RUN: not llvm-tblgen -gen-legalizer-check -I %p/../../include %s 2>&1 | FileCheck %s

// A list literal is a set, and a set has at least one member.  An empty list
// cannot be written in place -- TableGen has no type for it -- but it can
// arrive through a list template parameter, so the checker still needs to
// reject it rather than decode a membership no query can satisfy.

include "llvm/Target/LegalizerInfo.td"

def s32 : LegalizeNamedType<"LLT::scalar(32)">;

class WithSet<list<LegalizeNamedType> elems> : LegalizeRuleSet<["G_EMPTY"]> {
  let Rules = [CoreLegal<(CoreIn (Slot 0), elems)>];
}

def R : WithSet<[]>;

// CHECK: error: core rule: a set literal cannot be empty
