set(LLVM_LINK_COMPONENTS
  Support
  )

add_llvm_tool(gpumin-triton
  gpumin-triton.cpp
  PyAst.cpp
  Frontend.cpp
  TTIR.cpp
  )
