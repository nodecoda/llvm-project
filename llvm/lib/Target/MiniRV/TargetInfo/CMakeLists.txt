add_llvm_component_library(LLVMMiniRVInfo
  MiniRVTargetInfo.cpp

  LINK_COMPONENTS
  MC
  Support

  ADD_TO_COMPONENT
  MiniRV
  )
