add_llvm_component_group(RVMin)

set(LLVM_TARGET_DEFINITIONS RVMin.td)

llvm_tablegen(RVMinGenAsmMatcher.inc -gen-asm-matcher)
llvm_tablegen(RVMinGenAsmWriter.inc -gen-asm-writer)
llvm_tablegen(RVMinGenCallingConv.inc -gen-callingconv)
llvm_tablegen(RVMinGenCompressInstEmitter.inc -gen-compress-inst-emitter)
llvm_tablegen(RVMinGenDAGISel.inc -gen-dag-isel)
llvm_tablegen(RVMinGenDisassemblerTables.inc -gen-disassembler)
llvm_tablegen(RVMinGenInstrInfo.inc -gen-instr-info)
llvm_tablegen(RVMinGenMCCodeEmitter.inc -gen-emitter)
llvm_tablegen(RVMinGenMCPseudoLowering.inc -gen-pseudo-lowering)
llvm_tablegen(RVMinGenRegisterInfo.inc -gen-register-info)
llvm_tablegen(RVMinGenSDNodeInfo.inc -gen-sd-node-info)
llvm_tablegen(RVMinGenSubtargetInfo.inc -gen-subtarget)

add_public_tablegen_target(RVMinCommonTableGen)

add_llvm_target(RVMinCodeGen
  RVMinAsmPrinter.cpp
  RVMinCodeGenPassBuilder.cpp
  RVMinFrameLowering.cpp
  RVMinISelLowering.cpp
  RVMinISelDAGToDAG.cpp
  RVMinInstrInfo.cpp
  RVMinMCInstLower.cpp
  RVMinRegisterInfo.cpp
  RVMinSelectionDAGInfo.cpp
  RVMinSubtarget.cpp
  RVMinTargetMachine.cpp

  LINK_COMPONENTS
  AsmPrinter
  CodeGen
  CodeGenTypes
  Core
  MC
  RVMinDesc
  RVMinInfo
  Passes
  SelectionDAG
  Support
  Target
  TargetParser

  ADD_TO_COMPONENT
  RVMin
  )

add_subdirectory(AsmParser)
add_subdirectory(Disassembler)
add_subdirectory(MCTargetDesc)
add_subdirectory(TargetInfo)
