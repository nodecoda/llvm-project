//===- TinyTTOps.h - tinytt operations ------------------------*- C++ -*-===//

#ifndef TINY_TRITON_DIALECT_TINYTT_TINYTTTYPES_H_OPS
#define TINY_TRITON_DIALECT_TINYTT_TINYTTTYPES_H_OPS

#include "mlir/Bytecode/BytecodeOpInterface.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/Dialect.h"
#include "mlir/IR/OpDefinition.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"

#include "tiny/Dialect/TinyTT/TinyTTDialect.h"
#include "tiny/Dialect/TinyTT/TinyTTTypes.h"

#define GET_OP_CLASSES
#include "tiny/Dialect/TinyTT/TinyTTOps.h.inc"

#endif // TINY_TRITON_DIALECT_TINYTT_TINYTTTYPES_H_OPS
