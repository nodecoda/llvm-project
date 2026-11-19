//===- OpVerifiers.h - verifier logic shared by tinytt and tinyttg --------===//
//
// The ops are parameterised by dialect, so their verifiers are too: the checks
// below only look at operand/result types, never at which dialect owns the op.
// `tinyttg` adds one extra layer on top (every tensor must carry a layout).
//
//===----------------------------------------------------------------------===//

#ifndef TINY_TRITON_DIALECT_TINYCOMMON_OPVERIFIERS_H
#define TINY_TRITON_DIALECT_TINYCOMMON_OPVERIFIERS_H

#include "tiny/Dialect/TinyTT/TinyTTTypes.h"

#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/Operation.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"

namespace tiny::common {

inline mlir::RankedTensorType asRanked(mlir::Type Type) {
  return mlir::dyn_cast<mlir::RankedTensorType>(Type);
}

inline mlir::LogicalResult verifyMask(mlir::Operation *Op, mlir::Value Mask,
                                      mlir::RankedTensorType PtrType) {
  auto MaskType = asRanked(Mask.getType());
  if (!MaskType)
    return Op->emitOpError("mask must be a ranked tensor");
  if (!MaskType.getElementType().isInteger(1))
    return Op->emitOpError("mask element type must be i1");
  if (MaskType.getShape() != PtrType.getShape())
    return Op->emitOpError("mask shape must match the pointer tensor");
  return mlir::success();
}

inline mlir::LogicalResult verifyPtrAndPayload(mlir::Operation *Op,
                                               mlir::Value Ptr,
                                               mlir::Value Payload,
                                               mlir::RankedTensorType *PtrOut) {
  auto PtrType = asRanked(Ptr.getType());
  auto PayloadType = asRanked(Payload.getType());
  if (!PtrType || !PayloadType)
    return Op->emitOpError("expects ranked tensor types");
  auto Pointee = mlir::dyn_cast<tiny::tt::PtrType>(PtrType.getElementType());
  if (!Pointee)
    return Op->emitOpError("pointer operand must be a tensor of tiny pointers");
  if (PtrType.getShape() != PayloadType.getShape())
    return Op->emitOpError("payload shape must match the pointer tensor");
  if (Pointee.getPointee() != PayloadType.getElementType())
    return Op->emitOpError("payload element type must match the pointee type");
  if (PtrOut)
    *PtrOut = PtrType;
  return mlir::success();
}

inline mlir::LogicalResult verifyMakeRange(mlir::Operation *Op,
                                           mlir::Value Result,
                                           int64_t Axis) {
  auto ResultType = asRanked(Result.getType());
  if (!ResultType || ResultType.getRank() != 1)
    return Op->emitOpError("result must be a 1-D ranked tensor");
  if (!ResultType.getElementType().isInteger(32))
    return Op->emitOpError("result element type must be i32");
  if (Axis != 0)
    return Op->emitOpError("chapter 1 only supports axis = 0");
  return mlir::success();
}

inline mlir::LogicalResult verifySplat(mlir::Operation *Op, mlir::Value Src,
                                       mlir::Value Result) {
  auto ResultType = asRanked(Result.getType());
  if (!ResultType)
    return Op->emitOpError("result must be a ranked tensor");
  mlir::Type SrcType = Src.getType();
  if (mlir::isa<mlir::RankedTensorType>(SrcType))
    return Op->emitOpError("source must be a scalar, not a tensor");
  if (SrcType != ResultType.getElementType())
    return Op->emitOpError("source type must match the result element type");
  return mlir::success();
}

inline mlir::LogicalResult verifyAddPtr(mlir::Operation *Op, mlir::Value,
                                        mlir::Value Offset) {
  auto OffsetType = asRanked(Offset.getType());
  if (!OffsetType || !OffsetType.getElementType().isInteger())
    return Op->emitOpError("offset must be a tensor of integers");
  return mlir::success();
}

inline mlir::LogicalResult verifyLoad(mlir::Operation *Op, mlir::Value Ptr,
                                      mlir::Value Result) {
  return verifyPtrAndPayload(Op, Ptr, Result, nullptr);
}

inline mlir::LogicalResult verifyMaskedLoad(mlir::Operation *Op,
                                            mlir::Value Ptr, mlir::Value Mask,
                                            mlir::Value Other,
                                            mlir::Value Result) {
  mlir::RankedTensorType PtrType;
  if (mlir::failed(verifyPtrAndPayload(Op, Ptr, Result, &PtrType)))
    return mlir::failure();
  if (mlir::failed(verifyMask(Op, Mask, PtrType)))
    return mlir::failure();
  auto OtherType = asRanked(Other.getType());
  auto ResultType = asRanked(Result.getType());
  if (!OtherType)
    return Op->emitOpError("other must be a ranked tensor");
  if (OtherType.getShape() != PtrType.getShape())
    return Op->emitOpError("other shape must match the pointer tensor");
  if (OtherType.getElementType() != ResultType.getElementType())
    return Op->emitOpError("other element type must match the result");
  return mlir::success();
}

inline mlir::LogicalResult verifyStore(mlir::Operation *Op, mlir::Value Ptr,
                                       mlir::Value Value) {
  return verifyPtrAndPayload(Op, Ptr, Value, nullptr);
}

inline mlir::LogicalResult verifyMaskedStore(mlir::Operation *Op,
                                             mlir::Value Ptr,
                                             mlir::Value Value,
                                             mlir::Value Mask) {
  mlir::RankedTensorType PtrType;
  if (mlir::failed(verifyPtrAndPayload(Op, Ptr, Value, &PtrType)))
    return mlir::failure();
  return verifyMask(Op, Mask, PtrType);
}

inline mlir::LogicalResult verifyReduce(mlir::Operation *Op,
                                        mlir::Value Input,
                                        mlir::Value Result, int64_t Axis,
                                        llvm::StringRef Combine) {
  auto InputType = asRanked(Input.getType());
  if (!InputType)
    return Op->emitOpError("input must be a ranked tensor");
  if (InputType.getRank() != 1)
    return Op->emitOpError("tiny only reduces 1-D tensors");
  if (mlir::isa<mlir::RankedTensorType>(Result.getType()))
    return Op->emitOpError("reduce produces a scalar, not a tensor");
  if (InputType.getElementType() != Result.getType())
    return Op->emitOpError("input and result must share an element type");
  if (Axis != 0)
    return Op->emitOpError("tiny only reduces axis 0");

  if (Combine != "add" && Combine != "max" && Combine != "min")
    return Op->emitOpError("combine must be \"add\", \"max\" or \"min\"");
  return mlir::success();
}

inline mlir::LogicalResult verifyStoreScalar(mlir::Operation *Op,
                                             mlir::Value Ptr,
                                             mlir::Value Value) {
  auto PtrType = mlir::dyn_cast<tiny::tt::PtrType>(Ptr.getType());
  if (!PtrType)
    return Op->emitOpError("ptr must be a scalar !tinytt.ptr");
  if (mlir::isa<mlir::RankedTensorType>(Value.getType()))
    return Op->emitOpError("value must be a scalar, not a tensor");
  if (PtrType.getPointee() != Value.getType())
    return Op->emitOpError("pointee type must match the stored value");
  return mlir::success();
}

inline mlir::LogicalResult verifyTrans(mlir::Operation *Op,
                                       mlir::Value Input, mlir::Value Result,
                                       llvm::ArrayRef<int32_t> Order) {
  auto InputType = asRanked(Input.getType());
  auto ResultType = asRanked(Result.getType());
  if (!InputType || !ResultType)
    return Op->emitOpError("expects ranked tensor types");
  if (InputType.getElementType() != ResultType.getElementType())
    return Op->emitOpError("input and result must share an element type");

  int64_t Rank = InputType.getRank();
  if (static_cast<int64_t>(Order.size()) != Rank)
    return Op->emitOpError("order must have one entry per dimension");
  if (ResultType.getRank() != Rank)
    return Op->emitOpError("result rank must match the input rank");

  llvm::SmallVector<bool> Seen(Rank, false);
  llvm::SmallVector<int64_t> ExpectedShape(Rank);
  llvm::ArrayRef<int64_t> InputShape = InputType.getShape();
  for (int64_t I = 0; I < Rank; ++I) {
    int32_t Dim = Order[I];
    if (Dim < 0 || Dim >= Rank)
      return Op->emitOpError("order contains an out-of-range dimension");
    if (Seen[Dim])
      return Op->emitOpError("order must be a permutation");
    Seen[Dim] = true;
    ExpectedShape[I] = InputShape[Dim];
  }
  if (ResultType.getShape() != llvm::ArrayRef<int64_t>(ExpectedShape))
    return Op->emitOpError("result shape must equal the permuted input shape");
  return mlir::success();
}

inline mlir::LogicalResult verifyBroadcast(mlir::Operation *Op,
                                           mlir::Value Input,
                                           mlir::Value Result) {
  auto InputType = asRanked(Input.getType());
  auto ResultType = asRanked(Result.getType());
  if (!InputType || !ResultType)
    return Op->emitOpError("expects ranked tensor types");
  if (InputType.getElementType() != ResultType.getElementType())
    return Op->emitOpError("input and result must share an element type");
  if (InputType.getRank() != ResultType.getRank())
    return Op->emitOpError("broadcast must preserve the rank");

  llvm::ArrayRef<int64_t> InputShape = InputType.getShape();
  llvm::ArrayRef<int64_t> ResultShape = ResultType.getShape();
  for (int64_t I = 0, E = InputType.getRank(); I < E; ++I)
    if (InputShape[I] != 1 && InputShape[I] != ResultShape[I])
      return Op->emitOpError("dimension ")
             << I << " must be 1 or already match the result";
  return mlir::success();
}

inline mlir::LogicalResult verifyExpandDims(mlir::Operation *Op,
                                            mlir::Value Input,
                                            mlir::Value Result, int64_t Axis) {
  auto InputType = asRanked(Input.getType());
  auto ResultType = asRanked(Result.getType());
  if (!InputType || !ResultType)
    return Op->emitOpError("expects ranked tensor types");
  if (InputType.getElementType() != ResultType.getElementType())
    return Op->emitOpError("input and result must share an element type");

  int64_t ResultRank = ResultType.getRank();
  if (InputType.getRank() + 1 != ResultRank)
    return Op->emitOpError("expand_dims must add exactly one dimension");
  if (Axis < 0 || Axis >= ResultRank)
    return Op->emitOpError("axis is out of range for the result rank");

  llvm::SmallVector<int64_t> ExpectedShape;
  ExpectedShape.reserve(ResultRank);
  llvm::ArrayRef<int64_t> InputShape = InputType.getShape();
  int64_t Src = 0;
  for (int64_t I = 0; I < ResultRank; ++I)
    ExpectedShape.push_back(I == Axis ? 1 : InputShape[Src++]);
  if (ResultType.getShape() != llvm::ArrayRef<int64_t>(ExpectedShape))
    return Op->emitOpError("result shape must be the input shape with a 1 at axis");
  return mlir::success();
}

} // namespace tiny::common

#endif // TINY_TRITON_DIALECT_TINYCOMMON_OPVERIFIERS_H
