//===- TinyTTGAttrs.cpp - tinyttg layout syntax ----------------*- C++ -*-===//
//
// Both layouts print their integer arrays in Triton's shape syntax (`[1, 8]`)
// rather than MLIR's `array<i64: 1, 8>`.  The parser and the verifier are the
// chapter's contract: a layout that does not describe a permutation, or a
// tile with a non-positive extent, never reaches a `tinyttg` op.
//
//===----------------------------------------------------------------------===//

#include "tiny/Dialect/TinyTTG/TinyTTGAttrs.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "mlir/IR/DialectImplementation.h"

using namespace mlir;
using namespace tiny::ttg;

#define GET_ATTRDEF_CLASSES
#include "tiny/Dialect/TinyTTG/TinyTTGAttrs.cpp.inc"

// Attribute storage is private to this translation unit, so the dialect's
// protected `addAttributes` is reachable only from a member defined here.
void TinyTTGDialect::registerLayouts() { addAttributes<BlockedAttr, SharedAttr>(); }

namespace {

ParseResult parseI64List(AsmParser &parser, SmallVectorImpl<int64_t> &out) {
  return parser.parseCommaSeparatedList(AsmParser::Delimiter::Square, [&]() {
    int64_t value = 0;
    if (parser.parseInteger(value))
      return failure();
    out.push_back(value);
    return success();
  });
}

void printI64List(AsmPrinter &printer, ArrayRef<int64_t> values) {
  printer << '[';
  llvm::interleaveComma(values, printer);
  printer << ']';
}

/// Shared by both attributes: `order` must be a permutation of `0..rank-1`.
LogicalResult verifyOrder(function_ref<InFlightDiagnostic()> emitError,
                          ArrayRef<int64_t> order) {
  int64_t rank = order.size();
  SmallVector<bool> seen(rank, false);
  for (int64_t i = 0; i < rank; ++i) {
    int64_t dim = order[i];
    if (dim < 0 || dim >= rank || seen[dim])
      return emitError() << "order must be a permutation of 0.." << rank - 1;
    seen[dim] = true;
  }
  return success();
}

} // namespace

//===----------------------------------------------------------------------===//
// #tinyttg.blocked<sizePerThread = [...], threadsPerWarp = [...],
//                  warpsPerCTA = [...], order = [...]>
//===----------------------------------------------------------------------===//

Attribute BlockedAttr::parse(AsmParser &parser, Type) {
  SmallVector<int64_t> sizePerThread, threadsPerWarp, warpsPerCTA, order;

  if (parser.parseLess() || parser.parseKeyword("sizePerThread") ||
      parser.parseEqual() || parseI64List(parser, sizePerThread) ||
      parser.parseComma() || parser.parseKeyword("threadsPerWarp") ||
      parser.parseEqual() || parseI64List(parser, threadsPerWarp) ||
      parser.parseComma() || parser.parseKeyword("warpsPerCTA") ||
      parser.parseEqual() || parseI64List(parser, warpsPerCTA) ||
      parser.parseComma() || parser.parseKeyword("order") ||
      parser.parseEqual() || parseI64List(parser, order) ||
      parser.parseGreater())
    return {};

  return parser.getChecked<BlockedAttr>(parser.getContext(), sizePerThread,
                                        threadsPerWarp, warpsPerCTA, order);
}

void BlockedAttr::print(AsmPrinter &printer) const {
  printer << "<sizePerThread = ";
  printI64List(printer, getSizePerThread());
  printer << ", threadsPerWarp = ";
  printI64List(printer, getThreadsPerWarp());
  printer << ", warpsPerCTA = ";
  printI64List(printer, getWarpsPerCTA());
  printer << ", order = ";
  printI64List(printer, getOrder());
  printer << '>';
}

LogicalResult BlockedAttr::verify(function_ref<InFlightDiagnostic()> emitError,
                                  ArrayRef<int64_t> sizePerThread,
                                  ArrayRef<int64_t> threadsPerWarp,
                                  ArrayRef<int64_t> warpsPerCTA,
                                  ArrayRef<int64_t> order) {
  int64_t rank = sizePerThread.size();
  if (rank == 0)
    return emitError() << "a blocked layout needs at least one dimension";
  if (threadsPerWarp.size() != static_cast<size_t>(rank) ||
      warpsPerCTA.size() != static_cast<size_t>(rank) ||
      order.size() != static_cast<size_t>(rank))
    return emitError()
           << "sizePerThread, threadsPerWarp, warpsPerCTA and order must all "
              "have the tensor rank";

  for (int64_t i = 0; i < rank; ++i)
    if (sizePerThread[i] <= 0 || threadsPerWarp[i] <= 0 ||
        warpsPerCTA[i] <= 0)
      return emitError() << "layout extents must be positive";
  return verifyOrder(emitError, order);
}

//===----------------------------------------------------------------------===//
// #tinyttg.shared<offset = 0, order = [1, 0]>
//===----------------------------------------------------------------------===//

Attribute SharedAttr::parse(AsmParser &parser, Type) {
  int64_t offset = 0;
  SmallVector<int64_t> order;

  if (parser.parseLess() || parser.parseKeyword("offset") ||
      parser.parseEqual() || parser.parseInteger(offset) ||
      parser.parseComma() || parser.parseKeyword("order") ||
      parser.parseEqual() || parseI64List(parser, order) ||
      parser.parseGreater())
    return {};

  return parser.getChecked<SharedAttr>(parser.getContext(), offset, order);
}

void SharedAttr::print(AsmPrinter &printer) const {
  printer << "<offset = " << getOffset() << ", order = ";
  printI64List(printer, getOrder());
  printer << '>';
}

LogicalResult SharedAttr::verify(function_ref<InFlightDiagnostic()> emitError,
                                 int64_t offset, ArrayRef<int64_t> order) {
  if (offset < 0)
    return emitError() << "shared offset must be non-negative";
  if (order.empty())
    return emitError() << "a shared layout needs at least one dimension";
  return verifyOrder(emitError, order);
}
