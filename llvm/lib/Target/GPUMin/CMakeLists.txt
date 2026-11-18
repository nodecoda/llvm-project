add_llvm_component_group(GPUMin)

set(LLVM_TARGET_DEFINITIONS GPUMin.td)

llvm_tablegen(GPUMinGenAsmMatcher.inc -gen-asm-matcher)
llvm_tablegen(GPUMinGenAsmWriter.inc -gen-asm-writer)
llvm_tablegen(GPUMinGenCallingConv.inc -gen-callingconv)
llvm_tablegen(GPUMinGenDAGISel.inc -gen-dag-isel)
llvm_tablegen(GPUMinGenDisassemblerTables.inc -gen-disassembler)
llvm_tablegen(GPUMinGenInstrInfo.inc -gen-instr-info)
llvm_tablegen(GPUMinGenMCCodeEmitter.inc -gen-emitter)
llvm_tablegen(GPUMinGenMCPseudoLowering.inc -gen-pseudo-lowering)
llvm_tablegen(GPUMinGenRegisterInfo.inc -gen-register-info)
llvm_tablegen(GPUMinGenSDNodeInfo.inc -gen-sd-node-info)
llvm_tablegen(GPUMinGenSubtargetInfo.inc -gen-subtarget)

add_public_tablegen_target(GPUMinCommonTableGen)

add_llvm_target(GPUMinCodeGen
  GPUMinAsmPrinter.cpp
  GPUMinCodeGenPassBuilder.cpp
  GPUMinFrameLowering.cpp
  GPUMinISelLowering.cpp
  GPUMinISelDAGToDAG.cpp
  GPUMinInstrInfo.cpp
  GPUMinMCInstLower.cpp
  GPUMinRegisterInfo.cpp
  GPUMinSelectionDAGInfo.cpp
  GPUMinSubtarget.cpp
  GPUMinTargetMachine.cpp
  GPUMinTargetTransformInfo.cpp

  LINK_COMPONENTS
  Analysis
  AsmPrinter
  CodeGen
  CodeGenTypes
  Core
  MC
  GPUMinDesc
  GPUMinInfo
  Passes
  SelectionDAG
  Support
  Target
  TargetParser
  TransformUtils

  ADD_TO_COMPONENT
  GPUMin
  )

add_subdirectory(AsmParser)
add_subdirectory(Disassembler)
add_subdirectory(MCTargetDesc)
add_subdirectory(TargetInfo)
