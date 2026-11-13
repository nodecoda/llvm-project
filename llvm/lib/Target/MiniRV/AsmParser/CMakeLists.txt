add_llvm_component_library(LLVMMiniRVAsmParser
  MiniRVAsmParser.cpp

  LINK_COMPONENTS
  MC
  MCParser
  MiniRVDesc
  MiniRVInfo
  Support

  ADD_TO_COMPONENT
  MiniRV
  )
