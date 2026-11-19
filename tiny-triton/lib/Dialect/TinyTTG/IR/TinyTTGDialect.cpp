//===- TinyTTGDialect.cpp - tinyttg dialect --------------------*- C++ -*-===//

#include "tiny/Dialect/TinyTTG/TinyTTG.h"

#include "tiny/Dialect/TinyTT/TinyTTDialect.h"

using namespace mlir;
using namespace tiny::ttg;

#include "tiny/Dialect/TinyTTG/TinyTTGOpsDialect.cpp.inc"

void TinyTTGDialect::initialize() {
  addOperations<
#define GET_OP_LIST
#include "tiny/Dialect/TinyTTG/TinyTTGOps.cpp.inc"
      >();
  registerLayouts();
}
