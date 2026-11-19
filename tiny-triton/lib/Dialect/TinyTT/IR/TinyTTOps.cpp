//===- TinyTTOps.cpp - tinytt operation verifiers -------------*- C++ -*-===//
//
// The verifiers are the contract of chapter 1: they decide what "a TTIR
// program" means, independently of any later stage.  The checks themselves
// live in `TinyCommon/OpVerifiers.h` so chapter 3's `tinyttg` can share them.
//
//===----------------------------------------------------------------------===//

#include "tiny/Dialect/TinyTT/TinyTTOps.h"

#include "tiny/Dialect/TinyCommon/OpVerifiers.h"

#include "mlir/IR/OpImplementation.h"

using namespace mlir;
using namespace tiny::tt;

#define GET_OP_CLASSES
#include "tiny/Dialect/TinyTT/TinyTTOps.cpp.inc"

LogicalResult MakeRangeOp::verify() {
  return common::verifyMakeRange(getOperation(), getResult(), getAxis());
}

LogicalResult SplatOp::verify() {
  return common::verifySplat(getOperation(), getSrc(), getResult());
}

LogicalResult AddPtrOp::verify() {
  return common::verifyAddPtr(getOperation(), getPtr(), getOffset());
}

LogicalResult LoadOp::verify() {
  return common::verifyLoad(getOperation(), getPtr(), getResult());
}

LogicalResult MaskedLoadOp::verify() {
  return common::verifyMaskedLoad(getOperation(), getPtr(), getMask(),
                                  getOther(), getResult());
}

LogicalResult StoreOp::verify() {
  return common::verifyStore(getOperation(), getPtr(), getValue());
}

LogicalResult MaskedStoreOp::verify() {
  return common::verifyMaskedStore(getOperation(), getPtr(), getValue(),
                                   getMask());
}

LogicalResult ReduceOp::verify() {
  return common::verifyReduce(getOperation(), getInput(), getResult(),
                              getAxis(), getCombine());
}

LogicalResult TransOp::verify() {
  return common::verifyTrans(getOperation(), getInput(), getResult(),
                             getOrder());
}

LogicalResult BroadcastOp::verify() {
  return common::verifyBroadcast(getOperation(), getInput(), getResult());
}

LogicalResult ExpandDimsOp::verify() {
  return common::verifyExpandDims(getOperation(), getInput(), getResult(),
                                  getAxis());
}
