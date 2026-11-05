// RUN: llvm-tblgen -gen-legalizer-check -I %p/../../include %s | FileCheck %s --check-prefix=CHECK
// RUN: llvm-tblgen -gen-legalizer -I %p/../../include %s | FileCheck %s --check-prefix=CXX
// RUN: llvm-tblgen -gen-legalizer-core -I %p/../../include %s | FileCheck %s --check-prefix=CORE

// A set may be spelled at the point of use instead of being lifted to a named
// def.  The literal is a list, and TableGen only accepts a list as the last
// operand of a node, so the slot list stays explicit: `(CoreIn (Slot 0), [..])`.
// Every shape the named sets have is available: types, tuples, and memory
// descriptors.  A one-element list is still a set, not a bare type; the two
// spellings serialize differently and must not be conflated.

include "llvm/Target/LegalizerInfo.td"

def s32 : LegalizeNamedType<"LLT::scalar(32)">;
def s64 : LegalizeNamedType<"LLT::scalar(64)">;
def v2s64 : LegalizeNamedType<"LLT::fixed_vector(2, 64)">;
def p0 : LegalizeNamedType<"LLT::pointer(0, 64)">;

def Inline_Types : LegalizeRuleSet<["G_TYPES"]> {
  let Rules = [
    CoreLegal<(CoreIn (Slot 0), [s32, s64])>,
    CoreLegal<(CoreIn (Slot 0), [v2s64])>,
    CoreLegal<(CoreIn (Slot 0), s64)>
  ];
}

def Inline_Pairs : LegalizeRuleSet<["G_PAIRS"]> {
  let Rules = [
    CoreLegal<(CoreInPairs (Slot 0), (Slot 1), [[s32, s64], [s64, s32]])>
  ];
}

def Inline_Triples : LegalizeRuleSet<["G_TRIPLES"]> {
  let Rules = [
    CoreLegal<(CoreInTriples (Slot 0), (Slot 1), (Slot 2),
                             [[s32, s64, v2s64]])>
  ];
}

def Inline_MemDesc : LegalizeRuleSet<["G_LOAD"]> {
  let Rules = [
    CoreLegal<(CoreInMemDesc (Slot 0), (Slot 1), (MemSlot 0),
                             [LegalizeMemDesc<s32, p0, s32, 8>])>
  ];
}

// The checker counts the literal rules like any other.
// CHECK: "core_rules": 6
// CHECK: "legacy_rules": 0

// CXX: (Query.Types[0] == s32 || Query.Types[0] == s64)
// CXX: (Query.Types[0] == v2s64)
// CXX: (Query.Types[0] == s64)
// CXX: typePairInSet(0, 1, {{\{\{}}s32, s64}, {s64, s32}})
// CXX: typeTupleInSet(0, 1, 2, {{\{\{}}s32, s64, v2s64}})
// CXX: typePairAndMemDescInSet(0, 1, 0, {{\{\{}}s32, p0, s32, 8}})

// A one-member set is the type itself, whether it is written as a bracket or
// bare: the Core IR has one encoding for `(in s64)` and `(in [s64])`, so
// inlining a one-member set cannot change the artifact.
// CORE: "predicate":{"kind":"in","args":[{"kind":"slot","index":0},{"kind":"set","of":"Type","elems":["s32","s64"]}]}
// CORE: "predicate":{"kind":"in","args":[{"kind":"slot","index":0},{"kind":"type","value":"v2s64"}]}
// CORE: "predicate":{"kind":"in","args":[{"kind":"slot","index":0},{"kind":"type","value":"s64"}]}
// CORE: "kind":"inPairs"{{.*}}"of":"Type","arity":2{{.*}}"s32","s64"{{.*}}"s64","s32"
// CORE: "kind":"inTriples"{{.*}}"of":"Type","arity":3{{.*}}"s32","s64","v2s64"
// CORE: "kind":"memSlot","args":[{"kind":"int","value":0}]},{"kind":"set","of":"Type","arity":2{{.*}}"p0:64","s32",8
