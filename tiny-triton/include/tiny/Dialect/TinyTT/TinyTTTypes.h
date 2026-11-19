//===- TinyTTTypes.h - tinytt types ---------------------------*- C++ -*-===//

#ifndef TINY_TRITON_DIALECT_TINYTT_TINYTTTYPES_H
#define TINY_TRITON_DIALECT_TINYTT_TINYTTTYPES_H

#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/Types.h"

#include "tiny/Dialect/TinyTT/TinyTTDialect.h"

#define GET_TYPEDEF_CLASSES
#include "tiny/Dialect/TinyTT/TinyTTOpsTypes.h.inc"

#endif // TINY_TRITON_DIALECT_TINYTT_TINYTTTYPES_H
