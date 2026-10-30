// RUN: llvm-tblgen -gen-legalizer -I %p/../../include %s -o - | FileCheck %s
// RUN: llvm-tblgen -gen-legalizer-core -I %p/../../include %s -o - | FileCheck %s --check-prefix=CORE
// RUN: llvm-tblgen -gen-legalizer -I %p/../../include %s -o - | FileCheck %s --check-prefix=SIZE
// RUN: llvm-tblgen -gen-legalizer-core -I %p/../../include %s -o - | FileCheck %s --check-prefix=SIZECORE

// A guard that reads the query is a LegalityPredicate, a different C++ type
// from the cached bool legalFor(bool, ...) takes.  The two dags are therefore
// separate classes: a type predicate can go in a query dag and nowhere else,
// which is checked at parse time rather than when the target is compiled.

include "llvm/Target/LegalizerInfo.td"

def HasSSE1 : LegalizeFeature<"Subtarget.hasSSE1()">;
def s32 : LegalizeNamedType<"LLT::scalar(32)">;

def IsS32 : LegalizeTypePredicate<"typeIs(0, s32)">;

// !HasSSE1 && typeIs(0, s32)
def Custom : LegalizeQueryPredicateDag<(all_of (not HasSSE1), IsS32)>;

def Set : LegalizeRuleSet<["G_ADD"]> {
  let Rules = [LegalizeQueryGuardedCall<BuilderCustomIf, Custom, []>];
}

// CHECK: bool HasSSE1 = Subtarget.hasSSE1();
// CHECK: .customIf([=](const LegalityQuery &Query) { return !(HasSSE1) && typeIs(0, s32)(Query); })
//
// The trailing (Query) is the whole point.  typeIs() returns a
// LegalityPredicate, i.e. a std::function; writing it bare would let `&&`
// compare a std::function against bool through explicit operator bool, which
// is true for any function object, and the type test would silently vanish.

// CORE: "action":"Custom"
// CORE: "name":"typeIs"
// CORE: "args":[0,"s32"]

// sizeIs is the total width, so it is the one type predicate whose Core IR
// arguments are a number rather than an LLT.  It renders as a call like the
// rest -- a bare one would vanish into `explicit operator bool` -- and the
// number is a width, not a type, which is what lets a v4s32 and an s128 answer
// the same question.
def Size128 : LegalizeTypePredicate<"sizeIs(0, 128)">;
def SizeDag : LegalizeQueryPredicateDag<(all_of Size128)>;
def SizeSet : LegalizeRuleSet<["G_UNMERGE_VALUES"]> {
  let Rules = [LegalizeQueryGuardedCall<BuilderLegalIf, SizeDag, []>];
}

// SIZE: .legalIf([=](const LegalityQuery &Query) { return sizeIs(0, 128)(Query); })
// SIZECORE: "name":"sizeIs"
// SIZECORE: "args":[0,"128"]

