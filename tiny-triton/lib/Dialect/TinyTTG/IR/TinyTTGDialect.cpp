//===- TinyTTGDialect.cpp - tinyttg dialect --------------------*- C++ -*-===//

#include "tiny/Dialect/TinyTTG/TinyTTG.h"

#include "tiny/Dialect/TinyTT/TinyTTDialect.h"

#include "mlir/IR/OpImplementation.h"

using namespace mlir;
using namespace tiny::ttg;

#include "tiny/Dialect/TinyTTG/TinyTTGOpsDialect.cpp.inc"

namespace {

/// Print repeated layouts as `#blocked` / `#shared` aliases, the way Triton's
/// TTGIR does.  Without this, a 1024-element vecadd prints its layout on
/// every line and the IR stops being readable.
struct TinyTTGAsmDialectInterface : public OpAsmDialectInterface {
  using OpAsmDialectInterface::OpAsmDialectInterface;

  AliasResult getAlias(Attribute attr, raw_ostream &os) const override {
    if (isa<BlockedAttr>(attr)) {
      os << "blocked";
      return AliasResult::OverridableAlias;
    }
    if (isa<SharedAttr>(attr)) {
      os << "shared";
      return AliasResult::OverridableAlias;
    }
    return AliasResult::NoAlias;
  }
};

} // namespace

void TinyTTGDialect::initialize() {
  addOperations<
#define GET_OP_LIST
#include "tiny/Dialect/TinyTTG/TinyTTGOps.cpp.inc"
      >();
  registerLayouts();
  addInterface<TinyTTGAsmDialectInterface>();
}
