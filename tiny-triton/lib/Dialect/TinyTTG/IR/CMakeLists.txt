add_mlir_dialect_library(TinyTTGIR
  TinyTTGDialect.cpp
  TinyTTGAttrs.cpp
  TinyTTGOps.cpp

  ADDITIONAL_HEADER_DIRS
  ${TINY_TRITON_SOURCE_DIR}/include/tiny/Dialect/TinyTTG

  DEPENDS
  MLIRTinyTTGAttrsIncGen
  MLIRTinyTTGOpsIncGen
  MLIRTinyTTOpsIncGen

  LINK_LIBS PUBLIC
  MLIRIR
  MLIRSupport
  MLIRSideEffectInterfaces
  TinyTTIR
  )
