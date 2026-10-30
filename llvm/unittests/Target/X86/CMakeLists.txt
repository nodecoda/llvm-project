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

# TC-29/TC-31: the SUT must be the tuple recorded in the corpus JSON.  Two of
# the pinned inputs, the generated .inc and the Core IR, are build products, so
# the comparison cannot run at configure time; a build-time script writes a
# header with the recorded and observed values side by side and the test
# compares them field by field.
set(X86_LEGALIZER_INC ${LLVM_BINARY_DIR}/lib/Target/X86/X86LegalizerInfo.inc)
set(X86_LEGALIZER_CORE
    ${LLVM_BINARY_DIR}/lib/Target/X86/X86LegalizerInfo.core.json)
set(X86_LEGALIZER_ORACLE
    ${CMAKE_CURRENT_SOURCE_DIR}/X86LegalizerInfoReference.cpp)
set(X86_LEGALIZER_EMITTER
    ${LLVM_MAIN_SRC_DIR}/utils/TableGen/LegalizerInfoEmitter.cpp)
set(X86_LEGALIZER_LI_TD ${LLVM_MAIN_SRC_DIR}/include/llvm/Target/LegalizerInfo.td)

add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/x86-legalizer-pins.h
  COMMAND ${CMAKE_COMMAND}
    -DINC=${X86_LEGALIZER_INC}
    -DCORE=${X86_LEGALIZER_CORE}
    -DJSON=${X86_CTPOP_ATOMS_JSON}
    -DORACLE=${X86_LEGALIZER_ORACLE}
    -DEMITTER=${X86_LEGALIZER_EMITTER}
    -DLEGALIZERINFO_TD=${X86_LEGALIZER_LI_TD}
    -DX86_TD=${X86_LEGALIZER_TD}
    -DSRC_ROOT=${LLVM_MAIN_SRC_DIR}
    -DOUT=${CMAKE_CURRENT_BINARY_DIR}/x86-legalizer-pins.h
    -P ${CMAKE_CURRENT_SOURCE_DIR}/generate-legalizer-pins.cmake
  DEPENDS ${X86_LEGALIZER_INC} ${X86_LEGALIZER_CORE} ${X86_CTPOP_ATOMS_JSON}
          ${X86_LEGALIZER_TD} ${X86_LEGALIZER_ORACLE} ${X86_LEGALIZER_EMITTER}
          ${X86_LEGALIZER_LI_TD}
          ${CMAKE_CURRENT_SOURCE_DIR}/generate-legalizer-pins.cmake
  COMMENT "Checking GlobalISel legalizer pins"
  VERBATIM)

set(X86_CTPOP_GOLDEN ${CMAKE_CURRENT_SOURCE_DIR}/x86-ctpop-report.json)
add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/x86-ctpop-report.h
  COMMAND ${CMAKE_COMMAND} -DGOLDEN=${X86_CTPOP_GOLDEN}
    -DOUT=${CMAKE_CURRENT_BINARY_DIR}/x86-ctpop-report.h
    -P ${CMAKE_CURRENT_SOURCE_DIR}/generate-legalizer-golden.cmake
  DEPENDS ${X86_CTPOP_GOLDEN}
          ${CMAKE_CURRENT_SOURCE_DIR}/generate-legalizer-golden.cmake
  COMMENT "Embedding the G_CTPOP differential golden table"
  VERBATIM)

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
  x86-legalizer-pins.h
  x86-ctpop-report.h
  MachineSizeOptsTest.cpp
  TernlogTest.cpp
  X86LegalizerInfoDifferentialTest.cpp
  X86LegalizerInfoReference.cpp
  X86SelectionDAGTest.cpp
  )
