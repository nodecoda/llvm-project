add_llvm_component_group(MiniRV)

#===----------------------------------------------------------------------===//
# auto-td: the instruction records of this target are generated from the YAML
# corpus under auto-td/.  This is the only build-system hook the target needs;
# everything else below is the same MC-only scaffolding any target has.
#===----------------------------------------------------------------------===//

include(AutoTD)

llvm_autotd_generate(MiniRV
  TARGET_ROOT ${CMAKE_CURRENT_SOURCE_DIR}
  RISCV_OPCODES ${CMAKE_CURRENT_SOURCE_DIR}/auto-td/opcodes)

set(LLVM_TARGET_DEPENDS ${MiniRV_AUTOTD_INSTRINFO})
include_directories(${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_CURRENT_SOURCE_DIR})

set(LLVM_TARGET_DEFINITIONS MiniRV.td)

llvm_tablegen(MiniRVGenAsmMatcher.inc -gen-asm-matcher)
llvm_tablegen(MiniRVGenAsmWriter.inc -gen-asm-writer)
llvm_tablegen(MiniRVGenDisassemblerTables.inc -gen-disassembler)
llvm_tablegen(MiniRVGenInstrInfo.inc -gen-instr-info)
llvm_tablegen(MiniRVGenMCCodeEmitter.inc -gen-emitter)
llvm_tablegen(MiniRVGenRegisterInfo.inc -gen-register-info)
llvm_tablegen(MiniRVGenSubtargetInfo.inc -gen-subtarget)
llvm_tablegen(MiniRVGenCallingConv.inc -gen-callingconv)
llvm_tablegen(MiniRVGenDAGISel.inc -gen-dag-isel)
llvm_tablegen(MiniRVGenSDNodeInfo.inc -gen-sd-node-info)
llvm_tablegen(MiniRVGenMCPseudoLowering.inc -gen-pseudo-lowering)

add_public_tablegen_target(MiniRVCommonTableGen)

add_llvm_target(MiniRVCodeGen
  MiniRVAsmPrinter.cpp
  MiniRVCodeGenPassBuilder.cpp
  MiniRVFrameLowering.cpp
  MiniRVISelDAGToDAG.cpp
  MiniRVISelLowering.cpp
  MiniRVInstrInfo.cpp
  MiniRVMCInstLower.cpp
  MiniRVRegisterInfo.cpp
  MiniRVSelectionDAGInfo.cpp
  MiniRVSubtarget.cpp
  MiniRVTargetMachine.cpp
  LINK_COMPONENTS
  AsmPrinter
  CodeGen
  CodeGenTypes
  Core
  MC
  MiniRVDesc
  MiniRVInfo
  Option
  Passes
  SelectionDAG
  Support
  Target
  TargetParser
  ADD_TO_COMPONENT
  MiniRV
  )

add_subdirectory(MCTargetDesc)
add_subdirectory(TargetInfo)
add_subdirectory(AsmParser)
add_subdirectory(Disassembler)
