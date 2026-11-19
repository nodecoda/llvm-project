add_mlir_dialect(TinyTTGOps tinyttg)

set(LLVM_TARGET_DEFINITIONS TinyTTGAttrs.td)
mlir_tablegen(TinyTTGAttrs.h.inc -gen-attrdef-decls)
mlir_tablegen(TinyTTGAttrs.cpp.inc -gen-attrdef-defs)
add_mlir_dialect_tablegen_target(MLIRTinyTTGAttrsIncGen)
