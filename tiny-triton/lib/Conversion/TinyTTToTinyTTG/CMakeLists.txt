add_mlir_conversion_library(TinyTTToTinyTTG
  TinyTTToTinyTTG.cpp

  ADDITIONAL_HEADER_DIRS
  ${TINY_TRITON_SOURCE_DIR}/include/tiny/Conversion

  LINK_LIBS PUBLIC
  MLIRFuncDialect
  MLIRIR
  MLIRPass
  TinyTTGIR
  TinyTTIR
  )
