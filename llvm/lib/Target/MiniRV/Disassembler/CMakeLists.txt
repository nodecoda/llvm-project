add_llvm_component_library(LLVMMiniRVDisassembler
  MiniRVDisassembler.cpp

  LINK_COMPONENTS
  MC
  MCDisassembler
  MiniRVInfo
  Support

  ADD_TO_COMPONENT
  MiniRV
  )
