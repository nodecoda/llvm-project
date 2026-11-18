add_llvm_component_library(LLVMGPUMinAsmParser
  GPUMinAsmParser.cpp

  LINK_COMPONENTS
  MC
  MCParser
  GPUMinDesc
  GPUMinInfo
  Support

  ADD_TO_COMPONENT
  GPUMin
  )
