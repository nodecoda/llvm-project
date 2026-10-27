// RUN: not llvm-tblgen -gen-legalizer -I %p/../../include %s 2>&1 | FileCheck %s

// A feature is an ordinary TableGen def, so a typo in a dag leaf is caught by
// TableGen before -gen-legalizer runs, with a diagnostic on the .td line.

include "llvm/Target/LegalizerInfo.td"

def HasAVX : LegalizeFeature<"Subtarget.hasAVX()">;

def Bad : LegalizePredicateDag<(any_of HasAVXX, HasAVX)>;

// CHECK: error: Variable not defined: 'HasAVXX'