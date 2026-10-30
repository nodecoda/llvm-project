// RUN: llvm-tblgen -gen-legalizer -I %p/../../include %s 2>&1 | FileCheck %s

// A feature nothing refers to still becomes a bool in the generated
// constructor, where it is at best an unused variable.  Only reachable
// features are emitted, and the rest are reported.

include "llvm/Target/LegalizerInfo.td"

def Used : LegalizeFeature<"Subtarget.hasAVX()">;
def AlsoUsed : LegalizeFeature<"Subtarget.hasAVX2()">;
def Unused : LegalizeFeature<"Subtarget.hasAVX512()">;
def s8 : LegalizeNamedType<"LLT::scalar(8)">;

def Test : LegalizeRuleSet<["G_ADD"]> {
  let Rules = [LegalForIf<Used, [s8]>, LegalForIf<AlsoUsed, [s8]>];
}

// CHECK: warning: legalizer feature 'Unused' is never used and is not emitted
// CHECK: bool Used = Subtarget.hasAVX();
// CHECK: bool AlsoUsed = Subtarget.hasAVX2();
// CHECK-NOT: Unused