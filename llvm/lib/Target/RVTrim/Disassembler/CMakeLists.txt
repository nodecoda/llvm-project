add_llvm_component_library(LLVMRVTrimDisassembler
  RVTrimDisassembler.cpp

  LINK_COMPONENTS
  MC
  MCDisassembler
  RVTrimDesc
  RVTrimInfo
  Support

  ADD_TO_COMPONENT
  RVTrim
  )
