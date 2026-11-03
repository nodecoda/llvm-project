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
// A pointer whose width depends on the configuration is symbolic: `*` is bound
// by config.pointer_width, so the rule does not have to pick 32 or 64.
def p0_bound
    : LegalizeNamedType<"LLT::pointer(0, Is64Bit ? 64 : 32)">;
def v2p0
    : LegalizeNamedType<"LLT::fixed_vector(2, LLT::pointer(0, Is64Bit ? 64 : 32))">;
def Is64Bit : LegalizeFeature<"Subtarget.is64Bit()">;
// A select value type keeps its guards in Core IR instead of picking the type
// of whichever configuration generated the file.
def s_select : LegalizeSelectType<[Is64Bit], [s64], s32>;

def Test_STORE : LegalizeRuleSet<["G_STORE"]> {
  let Rules = [LegalForMemDesc<[LegalizeMemDesc<s32, p0, s32, 32>]>];
}

// The memory type contributes its size, not its spelling; the alignment is a
// separate number and stays independent of that size.
def Test_LOAD : LegalizeRuleSet<["G_LOAD"]> {
  let Rules = [LegalForMemDesc<[LegalizeMemDesc<s64, p0, v4s32, 32>]>];
}

// A pointer-valued memory type stays symbolic as well: the tuple compares the
// memory type's size, which is one (or two) configuration pointers wide.
def Test_PTRMEM : LegalizeRuleSet<["G_SEXTLOAD"]> {
  let Rules = [LegalForMemDesc<[LegalizeMemDesc<p0_bound, p0_bound, p0_bound, 32>,
                                LegalizeMemDesc<s64, p0_bound, v2p0, 64>]>];
}

// A value type may be a select type...
def Test_SELECT_VALUE : LegalizeRuleSet<["G_FPEXT"]> {
  let Rules = [LegalForMemDesc<[LegalizeMemDesc<s_select, s32, s32, 32>]>];
}

// ...but the memory axis still needs a literal size, so an unrepresentable
// tuple escapes as a whole instead of being approximated.
def Test_ESCAPE : LegalizeRuleSet<["G_ZEXTLOAD"]> {
  let Rules = [LegalForMemDesc<[LegalizeMemDesc<s32, s32, s_select, 32>]>];
}

// CHECK:      {"kind":"leaf","name":"typeIs","args":[0,"s32"]}
// CHECK-SAME: {"kind":"leaf","name":"typeIs","args":[1,"p0:32"]}
// CHECK-SAME: {"kind":"leaf","name":"memSize","args":[0,32,32]}
// CHECK-SAME: "ruleset": "Test_STORE"

// CHECK:      {"kind":"leaf","name":"typeIs","args":[0,"s64"]}
// CHECK-SAME: {"kind":"leaf","name":"typeIs","args":[1,"p0:32"]}
// CHECK-SAME: {"kind":"leaf","name":"memSize","args":[0,128,32]}
// CHECK-SAME: "ruleset": "Test_LOAD"

// CHECK:      {"kind":"leaf","name":"typeIs","args":[0,"p0:*"]}
// CHECK-SAME: {"kind":"leaf","name":"typeIs","args":[1,"p0:*"]}
// CHECK-SAME: {"kind":"leaf","name":"memSizePtr","args":[0,1,32]}
// CHECK-SAME: "ruleset": "Test_PTRMEM"

// CHECK:      {"kind":"leaf","name":"typeIs","args":[0,"s64"]}
// CHECK-SAME: {"kind":"leaf","name":"memSizePtr","args":[0,2,64]}
// CHECK-SAME: "ruleset": "Test_PTRMEM"

// CHECK:      {"select":{"cond":{"kind":"leaf","name":"hasFeature","args":["Subtarget.is64Bit()"]},"then":"s64","else":"s32"}}
// CHECK-SAME: "ruleset": "Test_SELECT_VALUE"

// CHECK: "kind": "nonliteral_type"
// CHECK-SAME: "ruleset": "Test_ESCAPE"

// CHECK-NOT: "name":"typeIs","args":[2,
