add_llvm_component_group(RVMin)

set(LLVM_TARGET_DEFINITIONS RVMin.td)

llvm_tablegen(RVMinGenRegisterInfo.inc -gen-register-info)
llvm_tablegen(RVMinGenInstrInfo.inc -gen-instr-info)
llvm_tablegen(RVMinGenSubtargetInfo.inc -gen-subtarget)

add_public_tablegen_target(RVMinCommonTableGen)

add_llvm_target(RVMinCodeGen
  RVMinAsmPrinter.cpp
  RVMinCodeGenPassBuilder.cpp
  RVMinFrameLowering.cpp
  RVMinISelLowering.cpp
  RVMinInstrInfo.cpp
  RVMinRegisterInfo.cpp
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

add_subdirectory(MCTargetDesc)
add_subdirectory(TargetInfo)
