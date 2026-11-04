//===- 04_preproc.td - TableGen preprocessor + include lab ---------------===//
// Run with:  llvm-tblgen -print-records 04_preproc.td
//            llvm-tblgen -DFEATURE_A -print-records 04_preproc.td
//            llvm-tblgen -E 04_preproc.td
include "04_preproc_inc.td"

#ifndef FEATURE_A
#define FEATURE_A
#endif

#ifdef FEATURE_A
def HasA;
#else
def NoA;
#endif

#ifndef FEATURE_B
def NoB;
#else
def HasB;
#endif

// Nested regions are allowed; #else flips the innermost region.
#ifdef FEATURE_B
  #ifdef FEATURE_A
  def BAndA;
  #else
  def BNotA;
  #endif
#endif
