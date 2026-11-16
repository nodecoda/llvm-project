add_llvm_component_library(LLVMRVMinDisassembler
  RVMinDisassembler.cpp

  LINK_COMPONENTS
  MC
  MCDisassembler
  RVMinInfo
  Support

  ADD_TO_COMPONENT
  RVMin
  )
