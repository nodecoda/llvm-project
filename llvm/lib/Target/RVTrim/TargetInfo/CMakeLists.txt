add_llvm_component_library(LLVMRVTrimInfo
  RVTrimTargetInfo.cpp

  LINK_COMPONENTS
  MC
  Support

  ADD_TO_COMPONENT
  RVTrim
  )
