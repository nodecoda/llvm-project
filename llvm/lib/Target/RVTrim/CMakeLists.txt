add_llvm_component_group(RVTrim)

#===----------------------------------------------------------------------===//
# auto-td: RV64IMA base instruction records from the opcode database.
#
# The generated file is included by RVTrimInstrInfo.td, so it must exist before
# any TableGen invocation that parses RVTrim.td.
#
# The backend implements two profiles: RV64IMA (default) and RV64IMA_Zve64x,
# which adds the integer vector extension (zve64x). Both are enforced by
# RVTrimFeatures::validateProfile().
#===----------------------------------------------------------------------===//

set(RVTRIM_AUTO_TD_DIR ${CMAKE_CURRENT_BINARY_DIR}/auto-td)
set(RVTRIM_AUTO_TD_GEN ${LLVM_MAIN_SRC_DIR}/utils/auto-td/auto-td-gen)
file(GLOB_RECURSE RVTRIM_AUTO_TD_INPUTS CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/auto-td/*.yaml
     ${CMAKE_CURRENT_SOURCE_DIR}/auto-td/opcodes/arg_lut.csv)

add_custom_command(
  OUTPUT ${RVTRIM_AUTO_TD_DIR}/AutoTdInstrInfo.inc
  COMMAND ${Python3_EXECUTABLE} ${RVTRIM_AUTO_TD_GEN}
          --target-root ${CMAKE_CURRENT_SOURCE_DIR}
          --riscv-opcodes ${CMAKE_CURRENT_SOURCE_DIR}/auto-td/opcodes
          --out-dir ${RVTRIM_AUTO_TD_DIR}
          --coverage ${RVTRIM_AUTO_TD_DIR}/AutoTdCoverage.md
  DEPENDS ${RVTRIM_AUTO_TD_INPUTS} ${RVTRIM_AUTO_TD_GEN}
  COMMENT "Generating RVTrim instruction records with auto-td"
  VERBATIM)

set(LLVM_TARGET_DEPENDS ${RVTRIM_AUTO_TD_DIR}/AutoTdInstrInfo.inc)
include_directories(${CMAKE_CURRENT_BINARY_DIR})

set(LLVM_TARGET_DEFINITIONS RVTrim.td)

llvm_tablegen(RVTrimGenAsmMatcher.inc -gen-asm-matcher)
llvm_tablegen(RVTrimGenAsmWriter.inc -gen-asm-writer)
llvm_tablegen(RVTrimGenCompressInstEmitter.inc -gen-compress-inst-emitter)
llvm_tablegen(RVTrimGenMacroFusion.inc -gen-macro-fusion-pred)
llvm_tablegen(RVTrimGenDAGISel.inc -gen-dag-isel)
llvm_tablegen(RVTrimGenDisassemblerTables.inc -gen-disassembler
              --specialize-decoders-per-bitwidth)
llvm_tablegen(RVTrimGenInstrInfo.inc -gen-instr-info)
llvm_tablegen(RVTrimGenMCCodeEmitter.inc -gen-emitter)
llvm_tablegen(RVTrimGenMCPseudoLowering.inc -gen-pseudo-lowering)
llvm_tablegen(RVTrimGenRegisterBank.inc -gen-register-bank)
llvm_tablegen(RVTrimGenRegisterInfo.inc -gen-register-info)
llvm_tablegen(RVTrimGenSearchableTables.inc -gen-searchable-tables)
llvm_tablegen(RVTrimGenSubtargetInfo.inc -gen-subtarget)
llvm_tablegen(RVTrimGenExegesis.inc -gen-exegesis)
llvm_tablegen(RVTrimGenSDNodeInfo.inc -gen-sd-node-info)

set(LLVM_TARGET_DEFINITIONS RVTrimGISel.td)
llvm_tablegen(RVTrimGenGlobalISel.inc -gen-global-isel)
llvm_tablegen(RVTrimGenO0PreLegalizeGICombiner.inc -gen-global-isel-combiner
              -combiners="RVTrimO0PreLegalizerCombiner")
llvm_tablegen(RVTrimGenPreLegalizeGICombiner.inc -gen-global-isel-combiner
              -combiners="RVTrimPreLegalizerCombiner")
llvm_tablegen(RVTrimGenPostLegalizeGICombiner.inc -gen-global-isel-combiner
              -combiners="RVTrimPostLegalizerCombiner")

set(LLVM_TARGET_DEFINITIONS RVTrimOptions.td)
llvm_tablegen(RVTrimOptions.inc -gen-opt-parser-defs)

set(LLVM_TARGET_DEFINITIONS MCTargetDesc/RVTrimMCOptions.td)
llvm_tablegen(RVTrimMCOptions.inc -gen-opt-parser-defs)

add_public_tablegen_target(RVTrimCommonTableGen)

add_llvm_target(RVTrimCodeGen
  RVTrimAsmPrinter.cpp
  RVTrimCallingConv.cpp
  RVTrimCodeGenPassBuilder.cpp
  RVTrimCodeGenPrepare.cpp
  RVTrimConstantPoolValue.cpp
  RVTrimDeadRegisterDefinitions.cpp
  RVTrimExpandPseudoAtomics.cpp
  RVTrimExpandPseudoBase.cpp
  RVTrimExpandPseudoPostRA.cpp
  RVTrimExpandPseudoPreEmit.cpp
  RVTrimExpandPseudoPreRA.cpp
  RVTrimFoldMemOffset.cpp
  RVTrimFrameLowering.cpp
  RVTrimGatherScatterLowering.cpp
  RVTrimIndirectBranchTracking.cpp
  RVTrimInsertReadWriteCSR.cpp
  RVTrimInsertVSETVLI.cpp
  RVTrimInsertWriteVXRM.cpp
  RVTrimInstrInfo.cpp
  RVTrimInterleavedAccess.cpp
  RVTrimISelDAGToDAG.cpp
  RVTrimISelLowering.cpp
  RVTrimLandingPadSetup.cpp
  RVTrimLateBranchOpt.cpp
  RVTrimLoadStoreOptimizer.cpp
  RVTrimMachineFunctionInfo.cpp
  RVTrimMakeCompressible.cpp
  RVTrimMachineScheduler.cpp
  RVTrimMergeBaseOffset.cpp
  RVTrimMoveMerger.cpp
  RVTrimOptWInstrs.cpp
  RVTrimPromoteConstant.cpp
  RVTrimPushPopOptimizer.cpp
  RVTrimQCRelaxMarking.cpp
  RVTrimRedundantCopyElimination.cpp
  RVTrimRegisterInfo.cpp
  RVTrimSelectionDAGInfo.cpp
  RVTrimSubtarget.cpp
  RVTrimTargetMachine.cpp
  RVTrimTargetObjectFile.cpp
  RVTrimTargetTransformInfo.cpp
  RVTrimVectorMaskDAGMutation.cpp
  RVTrimVectorPeephole.cpp
  RVTrimVLOptimizer.cpp
  RVTrimVMV0Elimination.cpp
  RVTrimVSETVLIInfoAnalysis.cpp
  RVTrimZacasABIFix.cpp
  RVTrimZilsdOptimizer.cpp
  GISel/RVTrimCallLowering.cpp
  GISel/RVTrimInlineAsmLowering.cpp
  GISel/RVTrimInstructionSelector.cpp
  GISel/RVTrimLegalizerInfo.cpp
  GISel/RVTrimPostLegalizerCombiner.cpp
  GISel/RVTrimO0PreLegalizerCombiner.cpp
  GISel/RVTrimPreLegalizerCombiner.cpp
  GISel/RVTrimRegisterBankInfo.cpp

  LINK_COMPONENTS
  Analysis
  AsmPrinter
  CodeGen
  CodeGenTypes
  Core
  GlobalISel
  IPO
  MC
  Option
  Passes
  RVTrimDesc
  RVTrimInfo
  Scalar
  SelectionDAG
  Support
  Target
  TargetParser
  TransformUtils
  Vectorize

  ADD_TO_COMPONENT
  RVTrim
  )

add_subdirectory(AsmParser)
add_subdirectory(Disassembler)
add_subdirectory(MCTargetDesc)
add_subdirectory(MCA)
add_subdirectory(TargetInfo)
