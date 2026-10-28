// RUN: llvm-tblgen -gen-legalizer-core -I %p/../../include %s | FileCheck %s

// A memdesc tuple {T0, T1, MemTy, Align} is not three types plus an integer.
// The C++ builder binds it with typePairAndMemDescInSet, whose MMO index is 0
// and whose memory descriptor is matched by size and minimum alignment.  The
// Core IR has to say the same thing, on its own mem axis.

include "llvm/Target/LegalizerInfo.td"

def s32 : LegalizeNamedType<"LLT::scalar(32)">;
def s64 : LegalizeNamedType<"LLT::scalar(64)">;
def v4s32 : LegalizeNamedType<"LLT::fixed_vector(4, 32)">;
def p0 : LegalizeNamedType<"LLT::pointer(0, 32)">;
// A pointer whose width depends on the subtarget has no literal form.
def p0_dyn
    : LegalizeNamedType<"Is64Bit ? LLT::pointer(0, 64) : LLT::pointer(0, 32)">;
def Is64Bit : LegalizeFeature<"Subtarget.is64Bit()">;

def Test_STORE : LegalizeRuleSet<["G_STORE"]> {
  let Rules = [LegalForMemDesc<[LegalizeMemDesc<s32, p0, s32, 32>]>];
}

// The memory type contributes its size, not its spelling; the alignment is a
// separate number and stays independent of that size.
def Test_LOAD : LegalizeRuleSet<["G_LOAD"]> {
  let Rules = [LegalForMemDesc<[LegalizeMemDesc<s64, p0, v4s32, 32>]>];
}

// An unrepresentable tuple escapes as a whole instead of being approximated.
def Test_ESCAPE : LegalizeRuleSet<["G_ZEXTLOAD"]> {
  let Rules = [LegalForMemDesc<[LegalizeMemDesc<s32, p0_dyn, s32, 32>]>];
}

// CHECK:      {"kind":"leaf","name":"typeIs","args":[0,"s32"]}
// CHECK-SAME: {"kind":"leaf","name":"typeIs","args":[1,"p0:32"]}
// CHECK-SAME: {"kind":"leaf","name":"memSize","args":[0,32,32]}
// CHECK-SAME: "ruleset": "Test_STORE"

// CHECK:      {"kind":"leaf","name":"typeIs","args":[0,"s64"]}
// CHECK-SAME: {"kind":"leaf","name":"typeIs","args":[1,"p0:32"]}
// CHECK-SAME: {"kind":"leaf","name":"memSize","args":[0,128,32]}
// CHECK-SAME: "ruleset": "Test_LOAD"

// CHECK: "kind": "nonliteral_type"
// CHECK-SAME: "ruleset": "Test_ESCAPE"

// CHECK-NOT: "name":"typeIs","args":[2,
