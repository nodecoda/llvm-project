add_llvm_component_library(LLVMRVTrimAsmParser
  RVTrimAsmParser.cpp

  LINK_COMPONENTS
  MC
  MCParser
  RVTrimDesc
  RVTrimInfo
  Support
  TargetParser

  ADD_TO_COMPONENT
  RVTrim
  )
