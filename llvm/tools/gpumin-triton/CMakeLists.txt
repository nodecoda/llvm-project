set(LLVM_LINK_COMPONENTS
  Core
  Support
  TargetParser
  )

add_llvm_tool(gpumin-triton
  gpumin-triton.cpp
  PyAst.cpp
  Frontend.cpp
  TTIR.cpp
  LLVMEmit.cpp
  )
