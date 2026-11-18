add_llvm_component_library(LLVMGPUMinDesc
  GPUMinAsmBackend.cpp
  GPUMinELFObjectWriter.cpp
  GPUMinInstPrinter.cpp
  GPUMinMCAsmInfo.cpp
  GPUMinMCCodeEmitter.cpp
  GPUMinMCTargetDesc.cpp

  LINK_COMPONENTS
  MC
  GPUMinInfo
  Support

  ADD_TO_COMPONENT
  GPUMin
  )
