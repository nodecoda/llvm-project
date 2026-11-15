add_llvm_component_library(LLVMRVMinDesc
  RVMinAsmBackend.cpp
  RVMinELFObjectWriter.cpp
  RVMinInstPrinter.cpp
  RVMinMCAsmInfo.cpp
  RVMinMCCodeEmitter.cpp
  RVMinMCTargetDesc.cpp

  LINK_COMPONENTS
  MC
  RVMinInfo
  Support

  ADD_TO_COMPONENT
  RVMin
  )
