add_llvm_component_library(LLVMRVTrimTargetMCA
  RVTrimCustomBehaviour.cpp

  LINK_COMPONENTS
  CodeGenTypes
  MC
  MCA
  MCParser
  RVTrimDesc
  RVTrimInfo
  Support
  TargetParser

  ADD_TO_COMPONENT
  RVTrim
  )
