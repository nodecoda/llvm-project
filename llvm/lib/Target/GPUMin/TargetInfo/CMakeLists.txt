add_llvm_component_library(LLVMGPUMinInfo
  GPUMinTargetInfo.cpp

  LINK_COMPONENTS
  MC
  Support
  TargetParser

  ADD_TO_COMPONENT
  GPUMin
  )
