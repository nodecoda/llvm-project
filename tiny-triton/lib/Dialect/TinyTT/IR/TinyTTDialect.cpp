//===- TinyTTDialect.cpp - tinytt dialect ---------------------*- C++ -*-===//

#include "tiny/Dialect/TinyTT/TinyTTDialect.h"
#include "tiny/Dialect/TinyTT/TinyTTOps.h"
#include "tiny/Dialect/TinyTT/TinyTTTypes.h"

using namespace mlir;
using namespace tiny::tt;

#include "tiny/Dialect/TinyTT/TinyTTOpsDialect.cpp.inc"

void TinyTTDialect::initialize() {
  addOperations<
#define GET_OP_LIST
#include "tiny/Dialect/TinyTT/TinyTTOps.cpp.inc"
      >();
  registerTypes();
}
