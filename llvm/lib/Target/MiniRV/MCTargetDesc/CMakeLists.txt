add_llvm_component_library(LLVMMiniRVDesc
  MiniRVAsmBackend.cpp
  MiniRVELFObjectWriter.cpp
  MiniRVInstPrinter.cpp
  MiniRVMCAsmInfo.cpp
  MiniRVMCCodeEmitter.cpp
  MiniRVMCTargetDesc.cpp

  LINK_COMPONENTS
  MC
  MiniRVInfo
  Support
  TargetParser

  ADD_TO_COMPONENT
  MiniRV
  )
