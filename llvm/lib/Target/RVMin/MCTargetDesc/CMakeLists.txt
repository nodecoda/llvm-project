add_llvm_component_library(LLVMRVMinDesc
  RVMinMCAsmInfo.cpp
  RVMinMCTargetDesc.cpp

  LINK_COMPONENTS
  MC
  RVMinInfo
  Support

  ADD_TO_COMPONENT
  RVMin
  )
