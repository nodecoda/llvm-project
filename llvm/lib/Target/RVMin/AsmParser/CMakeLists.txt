add_llvm_component_library(LLVMRVMinAsmParser
  RVMinAsmParser.cpp

  LINK_COMPONENTS
  MC
  MCParser
  RVMinDesc
  RVMinInfo
  Support

  ADD_TO_COMPONENT
  RVMin
  )
