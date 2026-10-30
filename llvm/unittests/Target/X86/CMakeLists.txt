include_directories(
  ${PROJECT_SOURCE_DIR}/lib/Target/X86
  ${PROJECT_BINARY_DIR}/lib/Target/X86
  )

# Frozen atom corpus: the JSON is the reviewed source of truth, and the
# generated header carries its count and canonical hash so the C++ atom table
# cannot drift from it silently.
set(X86_CTPOP_ATOMS_JSON ${CMAKE_CURRENT_SOURCE_DIR}/x86-ctpop-atoms.json)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             ${X86_CTPOP_ATOMS_JSON})
file(READ ${X86_CTPOP_ATOMS_JSON} X86_CTPOP_ATOMS_CONTENT)
string(JSON X86_CTPOP_ATOM_COUNT GET "${X86_CTPOP_ATOMS_CONTENT}" count)
string(JSON X86_CTPOP_ATOMS_FNV1A64 GET "${X86_CTPOP_ATOMS_CONTENT}"
       canonical_fnv1a64)
configure_file(x86-ctpop-atoms.h.in
               ${CMAKE_CURRENT_BINARY_DIR}/x86-ctpop-atoms.h @ONLY)
include_directories(${CMAKE_CURRENT_BINARY_DIR})


# Mutant tables for falsification.  A gtest cannot run tablegen, so each
# mutant is generated here from the committed .td and included by a separate
# test-local class.  string(REPLACE) is evaluated at configure time, hence the
# CMAKE_CONFIGURE_DEPENDS above; the STREQUAL guard catches a pattern that no
# longer matches, which would otherwise silently test the unmutated table.
set(X86_LEGALIZER_TD
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../lib/Target/X86/GISel/X86LegalizerInfo.td)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             ${X86_LEGALIZER_TD})

function(x86_add_legalizer_mutant name from to)
  file(READ ${X86_LEGALIZER_TD} _td)
  string(REPLACE "${from}" "${to}" _mut "${_td}")
  if(_mut STREQUAL _td)
    message(FATAL_ERROR
      "legalizer mutant ${name}: pattern did not match the .td; update it")
  endif()
  file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/${name}.td "${_mut}")
  set(LLVM_TARGET_DEFINITIONS ${CMAKE_CURRENT_BINARY_DIR}/${name}.td)
  tablegen(LLVM ${name}.inc -gen-legalizer)
  set(TABLEGEN_OUTPUT ${TABLEGEN_OUTPUT} PARENT_SCOPE)
endfunction()

# TC-23: drop the s32 pair from the HasPOPCNT rule.  Deleting a rule that no
# atom reaches would be a no-op, which is why this one is named explicitly.
x86_add_legalizer_mutant(X86LegalizerInfoMutant1
  "LegalForPairsIf<HasPOPCNT, [LegalizeTypePair<X86_s16, X86_s16>, LegalizeTypePair<X86_s32, X86_s32>]>"
  "LegalForPairsIf<HasPOPCNT, [LegalizeTypePair<X86_s16, X86_s16>]>")

# TC-24a: a transcription slip that writes HasAVX512 where the oracle wrote
# HasVPOPCNTDQ.  Only C5/C6 can see this.
x86_add_legalizer_mutant(X86LegalizerInfoMutant2
  "LegalForPairsIf<HasVPOPCNTDQ," "LegalForPairsIf<HasAVX512,")

# TC-24b: the analogous slip for the VLX-guarded 128/256-bit rule.  Only C6
# can see this.
x86_add_legalizer_mutant(X86LegalizerInfoMutant3
  "LegalForPairsIf<VPOPCNTAndVLX," "LegalForPairsIf<HasVLX,")

set(LLVM_LINK_COMPONENTS
  Analysis
  AsmParser
  CodeGen
  CodeGenTypes
  Core
  GlobalISel
  InstCombine
  MC
  MIRParser
  Passes
  SelectionDAG
  Support
  Target
  TargetParser
  X86CodeGen
  X86Desc
  X86Info
  )

add_llvm_unittest(X86Tests
  ${TABLEGEN_OUTPUT}
  MachineSizeOptsTest.cpp
  TernlogTest.cpp
  X86LegalizerInfoDifferentialTest.cpp
  X86LegalizerInfoReference.cpp
  X86SelectionDAGTest.cpp
  )
