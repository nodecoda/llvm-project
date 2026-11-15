add_llvm_component_library(LLVMRVMinInfo
  RVMinTargetInfo.cpp

  LINK_COMPONENTS
  MC
  Support
  TargetParser

  ADD_TO_COMPONENT
  RVMin
  )
