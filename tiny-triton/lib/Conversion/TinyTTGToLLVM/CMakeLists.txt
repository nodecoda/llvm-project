add_mlir_conversion_library(TinyTTGToLLVM
  TinyTTGToLLVM.cpp

  ADDITIONAL_HEADER_DIRS
  ${TINY_TRITON_SOURCE_DIR}/include/tiny/Conversion

  LINK_LIBS PUBLIC
  MLIRArithDialect
  MLIRFuncDialect
  MLIRIR
  MLIRLLVMDialect
  MLIRPass
  TinyTTGIR
  TinyTTIR
  )
