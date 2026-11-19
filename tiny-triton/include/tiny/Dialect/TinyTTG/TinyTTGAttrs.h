//===- TinyTTGAttrs.h - tinyttg layouts ------------------------*- C++ -*-===//

#ifndef TINY_TRITON_DIALECT_TINYTTG_TINYTTGATTRS_H
#define TINY_TRITON_DIALECT_TINYTTG_TINYTTGATTRS_H

#include "mlir/IR/Attributes.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/DialectImplementation.h"

#include "tiny/Dialect/TinyTTG/TinyTTGDialect.h"

#define GET_ATTRDEF_CLASSES
#include "tiny/Dialect/TinyTTG/TinyTTGAttrs.h.inc"

#endif // TINY_TRITON_DIALECT_TINYTTG_TINYTTGATTRS_H
