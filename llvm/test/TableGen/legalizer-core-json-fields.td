// RUN: llvm-tblgen -gen-legalizer-core -I %p/../../include %s | FileCheck %s

// A primitive entry is tagged with the schema it needs and carries no
// projected mutation: its mutation is derived from `to`, which is the only
// place the target constructor is written.  A legacy entry carries no tag and
// keeps the 0.3 mutation, so a 0.3-only target's artifacts do not move.
//
// The tag, not the presence of `op`, is what a reader keys on.  That is what
// makes a mixed file readable in parts instead of all-or-nothing.

include "llvm/Target/LegalizerInfo.td"

def Prim : LegalizeRuleSet<["G_PRIM"]> {
  let Rules = [CoreMore<(Slot 0), (Pow2)>];
}

def Legacy : LegalizeRuleSet<["G_LEG"]> {
  let Rules = [WidenScalarToNextPow2<0>];
}

// CHECK:      "version": "0.4",
// CHECK:      "def": "Prim"
// CHECK:      {"action":"MoreElements", "type_idx":0,
// CHECK-SAME: "schema": "0.4", "op": "more"
// CHECK-NOT:  "mutation"
// CHECK:      "def": "Legacy"
// CHECK:      {"action":"WidenScalar", "type_idx":0,
// CHECK-SAME: "mutation":{"target":0
