// RUN: not llvm-tblgen -gen-legalizer -I %p/../../include %s 2>&1 | FileCheck %s

// A builder is a record, not a string.  Before this, the spelling sat in
// LegalizeCall<"legalForr", ...> and nothing checked it: TableGen parsed it,
// -gen-legalizer wrote `.legalForr({s32});` into the fragment, and the typo
// surfaced hours later as "no member named legalForr in LegalizeRuleSet",
// pointing at LegalizerInfo.h instead of this line.  -gen-legalizer-core
// crashed on the same input instead, so the two backends disagreed about
// whether the rule existed at all.
//
// Naming a builder that does not exist is now a parse error, and so is a
// builder that exists but takes a guard when this one does not.

include "llvm/Target/LegalizerInfo.td"

def s32 : LegalizeNamedType<"LLT::scalar(32)">;
def HasSSE1 : LegalizeFeature<"Subtarget.hasSSE1()">;

// A guard on a builder that declares none used to emit `.lower(HasSSE1, )`,
// which no overload accepts.
def BadRule : LegalizeGuardedCall<BuilderLower, HasSSE1, []>;

def Bad : LegalizeRuleSet<["G_ADD"]> {
  let Rules = [BadRule];
}

// CHECK: error: legalizer builder 'lower' takes no guard
