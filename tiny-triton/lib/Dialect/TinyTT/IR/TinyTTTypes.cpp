//===- TinyTTTypes.cpp - tinytt types -------------------------*- C++ -*-===//

#include "tiny/Dialect/TinyTT/TinyTTTypes.h"

#include "llvm/ADT/TypeSwitch.h"
#include "mlir/IR/DialectImplementation.h"

using namespace mlir;
using namespace tiny::tt;

#define GET_TYPEDEF_CLASSES
#include "tiny/Dialect/TinyTT/TinyTTOpsTypes.cpp.inc"

// Type storage is private to this translation unit, so the dialect's
// protected `addTypes` is reachable only from a member defined here.
void TinyTTDialect::registerTypes() { addTypes<PtrType>(); }

//===----------------------------------------------------------------------===//
// !tinytt.ptr<pointee[, address-space]>
//===----------------------------------------------------------------------===//

Type PtrType::parse(AsmParser &parser) {
  Type pointee;
  int64_t addressSpace = kGlobalAddressSpace;

  if (parser.parseLess() || parser.parseType(pointee))
    return {};
  if (succeeded(parser.parseOptionalComma()) &&
      parser.parseInteger(addressSpace))
    return {};
  if (parser.parseGreater())
    return {};

  return PtrType::get(parser.getContext(), pointee, addressSpace);
}

void PtrType::print(AsmPrinter &printer) const {
  printer << "<" << getPointee();
  if (getAddressSpace() != kGlobalAddressSpace)
    printer << ", " << getAddressSpace();
  printer << ">";
}
