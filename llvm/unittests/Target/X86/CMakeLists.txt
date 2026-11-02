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

# Transcribe the two tables so the C++ corpus can be checked against the JSON
# field by field.  The hash above only covers the atom identities, so a sigma
# whose features drifted would otherwise be invisible.
set(X86_CTPOP_SIGMA_ROWS "")
string(JSON _n LENGTH "${X86_CTPOP_ATOMS_CONTENT}" sigmas)
math(EXPR _last "${_n} - 1")
foreach(_i RANGE ${_last})
  foreach(_k id triple features)
    string(JSON _${_k} GET "${X86_CTPOP_ATOMS_CONTENT}" sigmas ${_i} ${_k})
  endforeach()
  # string(JSON) hands booleans back as CMake's ON/OFF; emit C++ literals.
  foreach(_k is64 avx512 vpopcntdq vlx popcnt)
    string(JSON _v GET "${X86_CTPOP_ATOMS_CONTENT}" sigmas ${_i} ${_k})
    if(_v)
      set(_${_k} true)
    else()
      set(_${_k} false)
    endif()
  endforeach()
  string(APPEND X86_CTPOP_SIGMA_ROWS
    "  {\"${_id}\", \"${_triple}\", \"${_features}\", ${_is64}, ${_avx512}, ${_vpopcntdq}, ${_vlx}, ${_popcnt}},\n")
endforeach()

set(X86_CTPOP_TYPE_ROWS "")
string(JSON _n LENGTH "${X86_CTPOP_ATOMS_CONTENT}" types)
math(EXPR _last "${_n} - 1")
foreach(_i RANGE ${_last})
  foreach(_k id dst src)
    string(JSON _${_k} GET "${X86_CTPOP_ATOMS_CONTENT}" types ${_i} ${_k})
  endforeach()
  string(APPEND X86_CTPOP_TYPE_ROWS
    "  {\"${_id}\", \"${_dst}\", \"${_src}\"},\n")
endforeach()
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

# The rule index is a test aid, not a reader: -gen-legalizer-rule-index emits
# the same evaluators as -gen-legalizer-table, with the matching rule's
# position stored next to the step.  Nothing in lib/Target/X86 includes it.
# The domain the test walks is generated here too, so the 1397 queries are not
# copied into the test: the point emitter is the one selfCheck() uses.
set(LLVM_TARGET_DEFINITIONS ${X86_LEGALIZER_TD})
tablegen(LLVM X86LegalizerRuleIndex.inc -gen-legalizer-rule-index
         -legalizer-table-prefix=X86)
tablegen(LLVM X86LegalizerTablePointsForTest.inc -gen-legalizer-table-points
         -legalizer-table-prefix=X86)

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

# G005: fourteen mutants, each corrupting one rule in a different rule set.
# The categories are guard reversal (7, 8), action change (12, 13, 14), pair
# pruning (9, 10, 11) and feature-condition replacement (1..6).  Every pattern
# below is the minimal unique text for its rule; the STREQUAL guard in
# x86_add_legalizer_mutant turns a stale pattern into a configure error instead
# of a silently unmutated table.

# 1. X86_CTPOP: the 128/256-bit rule loses its VPOPCNTDQ half.  A target with
# VPOPCNTDQ but without VLX can see the difference.
x86_add_legalizer_mutant(X86LegalizerInfoMutant1
  "LegalForPairsIf<VPOPCNTAndVLX,"
  "LegalForPairsIf<HasVPOPCNTDQ,")

# 2. X86_CTLZ: HasLZCNT was written as HasPOPCNT.  Only a LZCNT target without
# POPCNT separates the two.
x86_add_legalizer_mutant(X86LegalizerInfoMutant2
  "LegalForPairsIf<HasLZCNT, [LegalizeTypePair<X86_s16, X86_s16>, LegalizeTypePair<X86_s32, X86_s32>]>"
  "LegalForPairsIf<HasPOPCNT, [LegalizeTypePair<X86_s16, X86_s16>, LegalizeTypePair<X86_s32, X86_s32>]>")

# 3. X86_CTTZ: HasBMI was written as HasLZCNT.
x86_add_legalizer_mutant(X86LegalizerInfoMutant3
  "LegalForPairsIf<HasBMI, [LegalizeTypePair<X86_s16, X86_s16>, LegalizeTypePair<X86_s32, X86_s32>]>"
  "LegalForPairsIf<HasLZCNT, [LegalizeTypePair<X86_s16, X86_s16>, LegalizeTypePair<X86_s32, X86_s32>]>")

# 4. X86_MUL: the pairwise s64 rule lost the DQI half of its guard.
x86_add_legalizer_mutant(X86LegalizerInfoMutant4
  "LegalForIf<DQIAndVLX, [X86_v2s64, X86_v4s64]>"
  "LegalForIf<HasVLX, [X86_v2s64, X86_v4s64]>")

# 5. X86_ADD: the 512-bit BWI rule was guarded by VLX instead.
x86_add_legalizer_mutant(X86LegalizerInfoMutant5
  "LegalForIf<HasBWI, [X86_v64s8, X86_v32s16]>"
  "LegalForIf<HasVLX, [X86_v64s8, X86_v32s16]>")

# 6. X86_IMPLICIT_DEF: s128 is no longer legal on every 64-bit target, only
# where AVX is available.
x86_add_legalizer_mutant(X86LegalizerInfoMutant6
  "LegalForIf<Is64Bit, [X86_s128]>"
  "LegalForIf<HasAVX, [X86_s128]>")

# 7. X86_SELECT: the !CMOV rule was inverted to a CMOV rule.
x86_add_legalizer_mutant(X86LegalizerInfoMutant7
  "LegalForPairsIf<NotCMOV, [LegalizeTypePair<X86_s8, X86_s32>]>"
  "LegalForPairsIf<HasCMOV, [LegalizeTypePair<X86_s8, X86_s32>]>")

# 8. X86_FNEG: the named predicate is replaced by its logical negation,
# !(UseX87 && !HasSSE1) == !UseX87 || HasSSE1.
x86_add_legalizer_mutant(X86LegalizerInfoMutant8
  "def UseX87NotSSE1 : LegalizePredicateDag<(all_of UseX87, (not HasSSE1))>;"
  "def UseX87NotSSE1 : LegalizePredicateDag<(any_of (not UseX87), HasSSE1)>;")

# 9. X86_CONCAT_VECTORS: the v32s8 <- v16s8 pair is dropped from the SSE1 rule.
x86_add_legalizer_mutant(X86LegalizerInfoMutant9
  "[LegalizeTypePair<X86_v32s8, X86_v16s8>, LegalizeTypePair<X86_v16s16, X86_v8s16>,"
  "[LegalizeTypePair<X86_v16s16, X86_v8s16>,")

# 10. X86_INSERT: the v64s8 <- v16s8 pair is dropped from the AVX512 rule.
x86_add_legalizer_mutant(X86LegalizerInfoMutant10
  "[LegalizeTypePair<X86_v64s8, X86_v16s8>,\n                     LegalizeTypePair<X86_v64s8, X86_v32s8>,"
  "[LegalizeTypePair<X86_v64s8, X86_v32s8>,")

# 11. X86_CTTZ_ZERO_POISON: the s16 pair is dropped from the unguarded rule.
x86_add_legalizer_mutant(X86LegalizerInfoMutant11
  "LegalForPairs<[LegalizeTypePair<X86_s16, X86_s16>, LegalizeTypePair<X86_s32, X86_s32>]>"
  "LegalForPairs<[LegalizeTypePair<X86_s32, X86_s32>]>")

# 12. X86_BSWAP: s32 is custom instead of legal.
x86_add_legalizer_mutant(X86LegalizerInfoMutant12
  "LegalFor<[X86_s32]>,"
  "CustomFor<[X86_s32]>,")

# 13. X86_IS_FPCLASS: the x87/64-bit lowering rule was changed to legal.
x86_add_legalizer_mutant(X86LegalizerInfoMutant13
  "LowerForPairsIf<Is64Bit, [LegalizeTypePair<X86_s1, X86_s32>, LegalizeTypePair<X86_s1, X86_s64>, LegalizeTypePair<X86_s1, X86_s80>]>"
  "LegalForPairsIf<Is64Bit, [LegalizeTypePair<X86_s1, X86_s32>, LegalizeTypePair<X86_s1, X86_s64>, LegalizeTypePair<X86_s1, X86_s80>]>")

# 14. X86_ANYEXT: the four legal result types are custom instead.
x86_add_legalizer_mutant(X86LegalizerInfoMutant14
  "LegalFor<[X86_s8, X86_s16, X86_s32, X86_s128]>,"
  "CustomFor<[X86_s8, X86_s16, X86_s32, X86_s128]>,")

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

# The independent oracle can be corrupted too.  Each copy starts as the frozen
# reference .cpp, has its class renamed, and has exactly one builder call
# replaced.  Both replacements must hit, or the mutant would silently test the
# unmodified oracle.
set(X86_LEGALIZER_REFERENCE_MUTANTS "")
function(x86_add_legalizer_reference_mutant name from to)
  file(READ ${X86_LEGALIZER_ORACLE} _ref)
  string(REPLACE "X86LegalizerInfoReference" "${name}" _renamed "${_ref}")
  if(_renamed STREQUAL _ref)
    message(FATAL_ERROR
      "reference mutant ${name}: class name did not match")
  endif()
  string(REPLACE "#include \"${name}.h\""
                 "#include \"X86LegalizerInfoReferenceMutants.h\""
                 _with_header "${_renamed}")
  if(_with_header STREQUAL _renamed)
    message(FATAL_ERROR
      "reference mutant ${name}: include line did not match")
  endif()
  string(REPLACE "${from}" "${to}" _mut "${_with_header}")
  if(_mut STREQUAL _with_header)
    message(FATAL_ERROR
      "reference mutant ${name}: rule pattern did not match the reference")
  endif()
  file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/${name}.cpp "${_mut}")
  set(X86_LEGALIZER_REFERENCE_MUTANTS
      ${X86_LEGALIZER_REFERENCE_MUTANTS}
      ${CMAKE_CURRENT_BINARY_DIR}/${name}.cpp PARENT_SCOPE)
endfunction()

# Reference mutant 1: an action change (legal -> lower) on G_CTLZ.
x86_add_legalizer_reference_mutant(X86LegalizerInfoReferenceMutant1
  ".legalFor(HasLZCNT, {{s16, s16}, {s32, s32}})"
  ".lowerFor(HasLZCNT, {{s16, s16}, {s32, s32}})")

# Reference mutant 2: a feature-guard transcription slip (VPOPCNTDQ ->
# AVX512) on G_CTPOP.
x86_add_legalizer_reference_mutant(X86LegalizerInfoReferenceMutant2
  ".legalFor(HasVPOPCNTDQ, {{v16s32, v16s32}, {v8s64, v8s64}})"
  ".legalFor(HasAVX512, {{v16s32, v16s32}, {v8s64, v8s64}})")

add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/x86-legalizer-pins.h
  COMMAND ${CMAKE_COMMAND}
    -DINC=${X86_LEGALIZER_INC}
    -DCORE=${X86_LEGALIZER_CORE}
    -DJSON=${X86_CTPOP_ATOMS_JSON}
    -DORACLE=${X86_LEGALIZER_ORACLE}
    -DSRC_ROOT=${LLVM_MAIN_SRC_DIR}
    -DOUT=${CMAKE_CURRENT_BINARY_DIR}/x86-legalizer-pins.h
    -P ${CMAKE_CURRENT_SOURCE_DIR}/generate-legalizer-pins.cmake
  DEPENDS ${X86_LEGALIZER_INC} ${X86_LEGALIZER_CORE} ${X86_CTPOP_ATOMS_JSON}
          ${X86_LEGALIZER_ORACLE}
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
  ${X86_LEGALIZER_REFERENCE_MUTANTS}
  x86-legalizer-pins.h
  x86-ctpop-report.h
  MachineSizeOptsTest.cpp
  TernlogTest.cpp
  X86LegalizerInfoDifferentialTest.cpp
  X86LegalizerInfoReference.cpp
  X86LegalizerInfoReferenceMutants.h
  X86SelectionDAGTest.cpp
  )

target_compile_definitions(X86Tests PRIVATE
  X86_LEGALIZER_TEST_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
  X86_LEGALIZER_CORE_JSON="${LLVM_BINARY_DIR}/lib/Target/X86/X86LegalizerInfo.core.json")
