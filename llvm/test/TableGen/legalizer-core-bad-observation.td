// RUN: not llvm-tblgen -gen-legalizer-check -I %p/../../include %s 2>&1 | FileCheck %s

// A lattice operator's observation is a Type.  A memory descriptor is a
// different axis, so `More` over MMODescrs[0] must not type-check even though
// both are "the first thing in the query".

include "llvm/Target/LegalizerInfo.td"

def R : LegalizeRuleSet<["G_ADD"]> {
  let Rules = [CoreMore<(MemSlot 0), (Pow2)>];
}

// CHECK: error: core rule: operator 'More' expects an observation of sort Type, got MemDesc
