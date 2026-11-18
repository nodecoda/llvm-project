add_llvm_component_library(LLVMGPUMinDisassembler
  GPUMinDisassembler.cpp

  LINK_COMPONENTS
  MC
  MCDisassembler
  GPUMinInfo
  Support

  ADD_TO_COMPONENT
  GPUMin
  )
