//===- TinyTTGOps.cpp - tinyttg verifiers ----------------------*- C++ -*-===//
//
// Chapter 3's contract: a `tinyttg` op is TTIR plus a thread mapping.  The
// shared TTIR checks live in `TinyCommon/OpVerifiers.h`; what is new here is
// that every tensor must carry a layout, and that a blocked layout must tile
// the tensor shape exactly.
//
//===----------------------------------------------------------------------===//

#include "tiny/Dialect/TinyTTG/TinyTTGOps.h"

#include "tiny/Conversion/LayoutGeometry.h"
#include "tiny/Dialect/TinyCommon/OpVerifiers.h"

#include "mlir/IR/Builders.h"
#include "mlir/IR/OpImplementation.h"

using namespace mlir;
using namespace tiny::ttg;

#define GET_OP_CLASSES
#include "tiny/Dialect/TinyTTG/TinyTTGOps.cpp.inc"

namespace {

/// Tensors must carry a layout; scalars and pointers are layout-free.
LogicalResult verifyTensorLayout(Operation *op, Value value, StringRef what) {
  auto tensor = dyn_cast<RankedTensorType>(value.getType());
  if (!tensor)
    return success();

  Attribute encoding = tensor.getEncoding();
  if (!encoding)
    return op->emitOpError() << what << " must carry a #tinyttg layout";

  if (auto blocked = dyn_cast<BlockedAttr>(encoding)) {
    int64_t rank = tensor.getRank();
    if (blocked.getRank() != rank)
      return op->emitOpError()
             << what << " layout rank does not match the tensor rank";
    int64_t threadsInLayout = tiny::layoutThreadsInLayout(blocked);
    for (int64_t dim = 0; dim < rank; ++dim) {
      int64_t extent = tensor.getShape()[dim];
      int64_t tile = blocked.getTileSize(dim);
      if (extent < 0)
        return op->emitOpError()
               << what << " must have a static shape in tinyttg";
      if (tile <= 0)
        return op->emitOpError() << what << ": dimension " << dim
                                 << " has a non-positive layout tile";
      // Extent 1 replicates and a shorter extent wraps (`tid % extent`);
      // everything else must tile exactly.  See LayoutGeometry.h.
      if (extent <= tile) {
        if (extent != 1 && threadsInLayout % extent != 0)
          return op->emitOpError()
                 << what << ": dimension " << dim << " of size " << extent
                 << " is smaller than layout tile " << tile
                 << " and does not divide the " << threadsInLayout
                 << " threads of the layout";
        continue;
      }
      if (extent % tile != 0)
        return op->emitOpError()
               << what << ": dimension " << dim << " of size " << extent
               << " is not divisible by layout tile " << tile;
    }
    return success();
  }

  if (auto shared = dyn_cast<SharedAttr>(encoding)) {
    if (static_cast<int64_t>(shared.getOrder().size()) != tensor.getRank())
      return op->emitOpError()
             << what << " shared layout rank does not match the tensor rank";
    return success();
  }

  return op->emitOpError() << what << " has an unknown layout attribute";
}

} // namespace

LogicalResult mlir::OpTrait::tiny::verifyLayouts(Operation *op) {
  for (Value operand : op->getOperands())
    if (failed(verifyTensorLayout(op, operand, "operand")))
      return failure();
  for (Value result : op->getResults())
    if (failed(verifyTensorLayout(op, result, "result")))
      return failure();
  return success();
}

//===----------------------------------------------------------------------===//
// The twelve TTIR operations, verified by the shared routines
//===----------------------------------------------------------------------===//

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

LogicalResult StoreScalarOp::verify() {
  return common::verifyStoreScalar(getOperation(), getPtr(), getValue());
}

LogicalResult Exp2Op::verify() {
  return common::verifyExp2(getOperation(), getInput(), getResult());
}

LogicalResult ExpandDimsOp::verify() {
  return common::verifyExpandDims(getOperation(), getInput(), getResult(),
                                  getAxis());
}

//===----------------------------------------------------------------------===//
// Thread mapping
//===----------------------------------------------------------------------===//

LogicalResult ConvertLayoutOp::verify() {
  auto srcType = dyn_cast<RankedTensorType>(getSrc().getType());
  auto resultType = dyn_cast<RankedTensorType>(getResult().getType());
  if (!srcType || !resultType)
    return emitOpError("src and result must be ranked tensors");
  if (srcType.getShape() != resultType.getShape())
    return emitOpError("src and result must have the same shape");
  if (srcType.getElementType() != resultType.getElementType())
    return emitOpError("src and result must have the same element type");
  return success();
}

LogicalResult LocalAllocOp::verify() {
  auto srcType = dyn_cast<RankedTensorType>(getSrc().getType());
  if (!srcType)
    return emitOpError("source must be a ranked tensor");
  auto ptrType = dyn_cast<tiny::tt::PtrType>(getResult().getType());
  if (!ptrType)
    return emitOpError("result must be a !tinytt.ptr");
  if (ptrType.getAddressSpace() != tiny::tt::PtrType::kSharedAddressSpace)
    return emitOpError("result must live in shared memory (address space 2)");
  if (ptrType.getPointee() != srcType.getElementType())
    return emitOpError("pointee type must match the element type");
  return success();
}

LogicalResult LocalStoreOp::verify() {
  auto valueType = dyn_cast<RankedTensorType>(getValue().getType());
  if (!valueType)
    return emitOpError("value must be a ranked tensor");
  auto ptrType = dyn_cast<tiny::tt::PtrType>(getPtr().getType());
  if (!ptrType)
    return emitOpError("ptr must be a !tinytt.ptr");
  if (ptrType.getAddressSpace() != tiny::tt::PtrType::kSharedAddressSpace)
    return emitOpError("ptr must live in shared memory (address space 2)");
  if (ptrType.getPointee() != valueType.getElementType())
    return emitOpError("pointee type must match the element type");
  return success();
}

LogicalResult LocalLoadOp::verify() {
  auto ptrType = dyn_cast<tiny::tt::PtrType>(getPtr().getType());
  if (!ptrType)
    return emitOpError("ptr must be a !tinytt.ptr");
  if (ptrType.getAddressSpace() != tiny::tt::PtrType::kSharedAddressSpace)
    return emitOpError("ptr must live in shared memory (address space 2)");
  auto resultType = dyn_cast<RankedTensorType>(getResult().getType());
  if (!resultType)
    return emitOpError("result must be a ranked tensor");
  if (ptrType.getPointee() != resultType.getElementType())
    return emitOpError("pointee type must match the element type");
  return success();
}
