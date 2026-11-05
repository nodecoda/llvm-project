include_directories(
  ${LLVM_MAIN_SRC_DIR}/lib/Target/AArch64
  ${LLVM_BINARY_DIR}/lib/Target/AArch64
  )

# The differential test consumes two generated fragments: the rule chain from
# the test-only .td, and the table reader's own query domain.  The domain is
# tuples-only because AArch64's scalable vectors cannot be probed with the
# synthetic shape domain the generic emitter walks by default.
set(AARCH64_LEGALIZER_TD
    ${LLVM_MAIN_SRC_DIR}/lib/Target/AArch64/GISel/AArch64LegalizerInfo.td)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             ${AARCH64_LEGALIZER_TD})
set(LLVM_TARGET_DEFINITIONS ${AARCH64_LEGALIZER_TD})
tablegen(LLVM AArch64LegalizerInfo.inc -gen-legalizer)
tablegen(LLVM AArch64LegalizerTablePointsForTest.inc
         -gen-legalizer-table-points -legalizer-table-prefix=AArch64
         -legalizer-table-tuples-only)
include_directories(${CMAKE_CURRENT_BINARY_DIR})

set(LLVM_LINK_COMPONENTS
  AArch64CodeGen
  AArch64Desc
  AArch64Info
  AArch64Utils
  Analysis
  AsmParser
  CodeGen
  CodeGenTypes
  Core
  GlobalISel
  MC
  MIRParser
  Passes
  SelectionDAG
  Support
  Target
  TargetParser
  )

add_llvm_target_unittest(AArch64Tests
  ${TABLEGEN_OUTPUT}
  AArch64LegalizerInfoDifferentialTest.cpp
  AArch64LegalizerInfoReference.cpp
  AArch64LegalizerInfoReference.h
  AArch64InstPrinterTest.cpp
  AddressingModes.cpp
  DecomposeStackOffsetTest.cpp
  InstSizes.cpp
  MatrixRegisterAliasing.cpp
  SMEAttributesTest.cpp
  AArch64CacheLineSizeTest.cpp
  AArch64RegisterInfoTest.cpp
  AArch64SVESchedPseudoTest.cpp
  AArch64SelectionDAGTest.cpp
  Immediates.cpp
  MCInstrAnalysisTest.cpp
  )
