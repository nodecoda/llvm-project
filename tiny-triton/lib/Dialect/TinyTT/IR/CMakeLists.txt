add_mlir_dialect_library(TinyTTIR
  TinyTTDialect.cpp
  TinyTTOps.cpp
  TinyTTTypes.cpp

  ADDITIONAL_HEADER_DIRS
  ${TINY_TRITON_SOURCE_DIR}/include/tiny/Dialect/TinyTT

  DEPENDS
  MLIRTinyTTOpsIncGen

  LINK_LIBS PUBLIC
  MLIRIR
  MLIRSupport
  MLIRSideEffectInterfaces
  )
