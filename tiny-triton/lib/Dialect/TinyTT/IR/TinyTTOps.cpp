//===- TinyTTOps.cpp - tinytt operation verifiers -------------*- C++ -*-===//
//
// The verifiers are the contract of chapter 1: they decide what "a TTIR
// program" means, independently of any later stage.
//
//===----------------------------------------------------------------------===//

#include "tiny/Dialect/TinyTT/TinyTTOps.h"

#include "mlir/IR/Builders.h"
#include "mlir/IR/OpImplementation.h"

using namespace mlir;
using namespace tiny::tt;

#define GET_OP_CLASSES
#include "tiny/Dialect/TinyTT/TinyTTOps.cpp.inc"

namespace {

RankedTensorType asRanked(Type type) {
  return dyn_cast<RankedTensorType>(type);
}

/// Shared checks for `mask` operands: i1 tensor with the pointer's shape.
LogicalResult verifyMask(Operation *op, Value mask,
                         RankedTensorType ptrType) {
  auto maskType = asRanked(mask.getType());
  if (!maskType)
    return op->emitOpError("mask must be a ranked tensor");
  if (!maskType.getElementType().isInteger(1))
    return op->emitOpError("mask element type must be i1");
  if (maskType.getShape() != ptrType.getShape())
    return op->emitOpError("mask shape must match the pointer tensor");
  return success();
}

/// `ptr` is a tensor of !tinytt.ptr; `other` has the same shape and the
/// pointee type.  Shared by load/masked_load and store/masked_store.
LogicalResult verifyPtrAndPayload(Operation *op, Value ptr, Value payload,
                                  RankedTensorType *ptrTypeOut) {
  auto ptrType = asRanked(ptr.getType());
  auto payloadType = asRanked(payload.getType());
  if (!ptrType || !payloadType)
    return op->emitOpError("expects ranked tensor types");
  auto pointee = dyn_cast<PtrType>(ptrType.getElementType());
  if (!pointee)
    return op->emitOpError("pointer operand must be a tensor of !tinytt.ptr");
  if (ptrType.getShape() != payloadType.getShape())
    return op->emitOpError("payload shape must match the pointer tensor");
  if (pointee.getPointee() != payloadType.getElementType())
    return op->emitOpError("payload element type must match the pointee type");
  if (ptrTypeOut)
    *ptrTypeOut = ptrType;
  return success();
}

} // namespace

//===----------------------------------------------------------------------===//
// Program structure
//===----------------------------------------------------------------------===//

LogicalResult MakeRangeOp::verify() {
  auto resultType = asRanked(getResult().getType());
  if (!resultType || resultType.getRank() != 1)
    return emitOpError("result must be a 1-D ranked tensor");
  if (!resultType.getElementType().isInteger(32))
    return emitOpError("result element type must be i32");
  if (getAxis() != 0)
    return emitOpError("chapter 1 only supports axis = 0");
  return success();
}

//===----------------------------------------------------------------------===//
// Value construction
//===----------------------------------------------------------------------===//

LogicalResult SplatOp::verify() {
  auto resultType = asRanked(getResult().getType());
  if (!resultType)
    return emitOpError("result must be a ranked tensor");
  Type srcType = getSrc().getType();
  if (isa<RankedTensorType>(srcType))
    return emitOpError("source must be a scalar, not a tensor");
  if (srcType != resultType.getElementType())
    return emitOpError("source type must match the result element type");
  return success();
}

//===----------------------------------------------------------------------===//
// Memory
//===----------------------------------------------------------------------===//

LogicalResult AddPtrOp::verify() {
  auto offsetType = asRanked(getOffset().getType());
  if (!offsetType || !offsetType.getElementType().isInteger())
    return emitOpError("offset must be a tensor of integers");
  return success();
}

LogicalResult LoadOp::verify() {
  return verifyPtrAndPayload(*this, getPtr(), getResult(), nullptr);
}

LogicalResult MaskedLoadOp::verify() {
  RankedTensorType ptrType;
  if (failed(verifyPtrAndPayload(*this, getPtr(), getResult(), &ptrType)))
    return failure();
  if (failed(verifyMask(*this, getMask(), ptrType)))
    return failure();
  auto otherType = asRanked(getOther().getType());
  auto resultType = asRanked(getResult().getType());
  if (!otherType)
    return emitOpError("other must be a ranked tensor");
  if (otherType.getShape() != ptrType.getShape())
    return emitOpError("other shape must match the pointer tensor");
  if (otherType.getElementType() != resultType.getElementType())
    return emitOpError("other element type must match the result");
  return success();
}

LogicalResult StoreOp::verify() {
  return verifyPtrAndPayload(*this, getPtr(), getValue(), nullptr);
}

LogicalResult MaskedStoreOp::verify() {
  RankedTensorType ptrType;
  if (failed(verifyPtrAndPayload(*this, getPtr(), getValue(), &ptrType)))
    return failure();
  return verifyMask(*this, getMask(), ptrType);
}

//===----------------------------------------------------------------------===//
// Shape-changing
//===----------------------------------------------------------------------===//

LogicalResult ReduceOp::verify() {
  auto inputType = asRanked(getInput().getType());
  auto resultType = asRanked(getResult().getType());
  if (!inputType || !resultType)
    return emitOpError("expects ranked tensor types");
  if (inputType.getElementType() != resultType.getElementType())
    return emitOpError("input and result must share an element type");

  StringRef combine = getCombine();
  if (combine != "add" && combine != "max" && combine != "min")
    return emitOpError("combine must be \"add\", \"max\" or \"min\"");

  int64_t rank = inputType.getRank();
  int64_t axis = getAxis();
  if (axis < 0 || axis >= rank)
    return emitOpError("axis is out of range for the input rank");
  if (resultType.getRank() != rank)
    return emitOpError("result rank must match the input rank");

  SmallVector<int64_t> expectedShape(inputType.getShape());
  expectedShape[axis] = 1;
  if (resultType.getShape() != ArrayRef<int64_t>(expectedShape))
    return emitOpError("result shape must match the input with axis set to 1");
  return success();
}

LogicalResult TransOp::verify() {
  auto inputType = asRanked(getInput().getType());
  auto resultType = asRanked(getResult().getType());
  if (!inputType || !resultType)
    return emitOpError("expects ranked tensor types");
  if (inputType.getElementType() != resultType.getElementType())
    return emitOpError("input and result must share an element type");

  int64_t rank = inputType.getRank();
  ArrayRef<int32_t> order = getOrder();
  if (static_cast<int64_t>(order.size()) != rank)
    return emitOpError("order must have one entry per dimension");
  if (resultType.getRank() != rank)
    return emitOpError("result rank must match the input rank");

  SmallVector<bool> seen(rank, false);
  SmallVector<int64_t> expectedShape(rank);
  ArrayRef<int64_t> inputShape = inputType.getShape();
  for (int64_t i = 0; i < rank; ++i) {
    int32_t dim = order[i];
    if (dim < 0 || dim >= rank)
      return emitOpError("order contains an out-of-range dimension");
    if (seen[dim])
      return emitOpError("order must be a permutation");
    seen[dim] = true;
    expectedShape[i] = inputShape[dim];
  }
  if (resultType.getShape() != ArrayRef<int64_t>(expectedShape))
    return emitOpError("result shape must equal the permuted input shape");
  return success();
}

LogicalResult BroadcastOp::verify() {
  auto inputType = asRanked(getInput().getType());
  auto resultType = asRanked(getResult().getType());
  if (!inputType || !resultType)
    return emitOpError("expects ranked tensor types");
  if (inputType.getElementType() != resultType.getElementType())
    return emitOpError("input and result must share an element type");
  if (inputType.getRank() != resultType.getRank())
    return emitOpError("broadcast must preserve the rank");

  ArrayRef<int64_t> inputShape = inputType.getShape();
  ArrayRef<int64_t> resultShape = resultType.getShape();
  for (int64_t i = 0, e = inputType.getRank(); i < e; ++i) {
    if (inputShape[i] != 1 && inputShape[i] != resultShape[i])
      return emitOpError("dimension ")
             << i << " must be 1 or already match the result";
  }
  return success();
}

LogicalResult ExpandDimsOp::verify() {
  auto inputType = asRanked(getInput().getType());
  auto resultType = asRanked(getResult().getType());
  if (!inputType || !resultType)
    return emitOpError("expects ranked tensor types");
  if (inputType.getElementType() != resultType.getElementType())
    return emitOpError("input and result must share an element type");

  int64_t resultRank = resultType.getRank();
  if (inputType.getRank() + 1 != resultRank)
    return emitOpError("expand_dims must add exactly one dimension");

  int64_t axis = getAxis();
  if (axis < 0 || axis >= resultRank)
    return emitOpError("axis is out of range for the result rank");

  SmallVector<int64_t> expectedShape;
  expectedShape.reserve(resultRank);
  ArrayRef<int64_t> inputShape = inputType.getShape();
  int64_t src = 0;
  for (int64_t i = 0; i < resultRank; ++i)
    expectedShape.push_back(i == axis ? 1 : inputShape[src++]);
  if (resultType.getShape() != ArrayRef<int64_t>(expectedShape))
    return emitOpError("result shape must be the input shape with a 1 at axis");
  return success();
}
