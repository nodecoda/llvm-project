//===- TinyTTGToLLVM.cpp - TTGIR -> LLVM dialect ----------------*- C++ -*-===//
//
// Chapter 5 is where the SSA graph really changes: a `tensor<1024xf32>` stops
// being one value and becomes 32 scalar values, one per thread.  The pass
// therefore keeps a lane vector per TTIR value and emits `llvm` dialect ops
// directly, in the same order as the source.
//
// The kernel ABI is the one chapter 24 of the GPUMin course defines: no formal
// parameters, the argument block address arrives through `llvm.gpumin.args`.
//
//===----------------------------------------------------------------------===//

#include "tiny/Conversion/TinyTTGToLLVM.h"

#include "tiny/Conversion/LayoutGeometry.h"
#include "tiny/Conversion/TargetInfo.h"
#include "tiny/Dialect/TinyTT/TinyTTTypes.h"
#include "tiny/Dialect/TinyTTG/TinyTTGOps.h"

#include <algorithm>

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/Pass/Pass.h"

using namespace mlir;

namespace {

/// One TTIR value after scalarisation.  A scalar has one element, a tensor has
/// one element per value the thread holds.
struct LaneValue {
  SmallVector<Value> elts;

  bool isScalar() const { return elts.size() == 1; }
};

/// Elements one thread holds for `type`.  Chapter 7 moved the arithmetic to
/// `LayoutGeometry.h` so that the TT->TTG pass and this pass cannot drift:
/// extent 1 counts as one repeat, an extent smaller than a tile counts as one
/// repeat too.
FailureOr<int64_t> elementsPerThread(RankedTensorType type) {
  auto blocked = dyn_cast<tiny::ttg::BlockedAttr>(type.getEncoding());
  if (!blocked)
    return failure();
  return tiny::layoutElementsPerThread(blocked, type.getShape());
}

/// Number of threads that participate in the layout.  This is the number of
/// shared slots a block-wide reduction needs, independent of how many
/// elements each thread happens to hold.
FailureOr<int64_t> threadsInLayout(RankedTensorType type) {
  auto blocked = dyn_cast<tiny::ttg::BlockedAttr>(type.getEncoding());
  if (!blocked)
    return failure();
  return tiny::layoutThreadsInLayout(blocked);
}

class FuncLowerer {
public:
  FuncLowerer(func::FuncOp source, ModuleOp module)
      : source(source), module(module), builder(source.getContext()) {}

  LogicalResult run();

private:
  Value constant(int64_t value);
  Value intrinsic(StringRef name);
  Value intrinsic(StringRef name, Type result, ValueRange args);
  FailureOr<Value> argumentValue(unsigned index, Type type);

  LogicalResult lowerOp(Operation *op);
  LogicalResult lowerConstant(arith::ConstantOp op);
  LogicalResult lowerBinary(Operation *op, StringRef name);
  LogicalResult lowerMakeRange(tiny::ttg::MakeRangeOp op);
  LogicalResult lowerSplat(tiny::ttg::SplatOp op);
  LogicalResult lowerAddPtr(tiny::ttg::AddPtrOp op);
  LogicalResult lowerLoad(tiny::ttg::LoadOp op);
  LogicalResult lowerStore(tiny::ttg::StoreOp op);
  LogicalResult lowerStoreScalar(tiny::ttg::StoreScalarOp op);
  LogicalResult lowerReduce(tiny::ttg::ReduceOp op);
  LogicalResult lowerExpandDims(tiny::ttg::ExpandDimsOp op);
  LogicalResult lowerBroadcast(tiny::ttg::BroadcastOp op);

  /// `tid` clamped to the threads the layout uses.  A 16-element layout only
  /// owns half a block, and the other half repeats the same coordinates.
  Value threadIndexInLayout(const tiny::ttg::BlockedAttr &blocked);
  /// Coordinate along `dim` of one register of `type`.
  Value coordValue(RankedTensorType type, const tiny::ElementPosition &position,
                   int64_t dim, Value linear);

  LaneValue lookup(Value value) const { return env.lookup(value); }

  func::FuncOp source;
  ModuleOp module;
  OpBuilder builder;
  DenseMap<Value, LaneValue> env;
  Value tid;
  Type ptrTy;
  Type sharedPtrTy;
  int64_t sharedOffset = 0;
  tiny::TargetInfo target;
};

Value FuncLowerer::constant(int64_t value) {
  return builder.create<LLVM::ConstantOp>(source.getLoc(), builder.getI32Type(),
                                          builder.getI32IntegerAttr(value));
}

Value FuncLowerer::intrinsic(StringRef name) {
  return intrinsic(name, builder.getI32Type(), ValueRange{});
}

Value FuncLowerer::intrinsic(StringRef name, Type result, ValueRange args) {
  return builder
      .create<LLVM::CallIntrinsicOp>(source.getLoc(), result,
                                     builder.getStringAttr(name), args)
      .getResults();
}

FailureOr<Value> FuncLowerer::argumentValue(unsigned index, Type type) {
  Location loc = source.getLoc();
  // The argument block is a 32-bit word array; `llvm.gpumin.args` hands us its
  // address.  Word `i` holds the i-th formal parameter.
  Value base = intrinsic("llvm.gpumin.args");
  Value offset = constant(4 * static_cast<int64_t>(index));
  Value address = builder.create<LLVM::AddOp>(loc, base, offset);
  Value slot = builder.create<LLVM::IntToPtrOp>(loc, ptrTy, address);
  Value raw = builder.create<LLVM::LoadOp>(loc, builder.getI32Type(), slot);
  if (isa<tiny::tt::PtrType>(type))
    return builder.create<LLVM::IntToPtrOp>(loc, ptrTy, raw).getResult();
  if (type.isInteger(32))
    return raw;
  return source.emitError() << "kernel argument " << index
                            << " must be a pointer or i32, got " << type;
}

LogicalResult FuncLowerer::lowerConstant(arith::ConstantOp op) {
  if (isa<RankedTensorType>(op.getType()))
    return op.emitError() << "tensor constants are not supported; the frontend "
                             "emits tinytt.splat instead";
  Attribute value = op.getValue();
  Value lowered;
  if (auto integer = dyn_cast<IntegerAttr>(value))
    lowered = builder.create<LLVM::ConstantOp>(op.getLoc(), integer.getType(),
                                               integer);
  else if (auto fp = dyn_cast<FloatAttr>(value))
    lowered = builder.create<LLVM::ConstantOp>(op.getLoc(), fp.getType(), fp);
  else
    return op.emitError() << "unsupported constant kind";
  env[op.getResult()] = LaneValue{{lowered}};
  return success();
}

LogicalResult FuncLowerer::lowerBinary(Operation *op, StringRef name) {
  LaneValue lhs = lookup(op->getOperand(0));
  LaneValue rhs = lookup(op->getOperand(1));

  size_t count = std::max(lhs.elts.size(), rhs.elts.size());
  LaneValue result;
  for (size_t i = 0; i < count; ++i) {
    Value a = lhs.elts[std::min(i, lhs.elts.size() - 1)];
    Value b = rhs.elts[std::min(i, rhs.elts.size() - 1)];
    Value value;
    if (name == "arith.addi")
      value = builder.create<LLVM::AddOp>(op->getLoc(), a, b);
    else if (name == "arith.subi")
      value = builder.create<LLVM::SubOp>(op->getLoc(), a, b);
    else if (name == "arith.muli")
      value = builder.create<LLVM::MulOp>(op->getLoc(), a, b);
    else if (name == "arith.addf")
      value = builder.create<LLVM::FAddOp>(op->getLoc(), a, b);
    else if (name == "arith.subf")
      value = builder.create<LLVM::FSubOp>(op->getLoc(), a, b);
    else if (name == "arith.mulf")
      value = builder.create<LLVM::FMulOp>(op->getLoc(), a, b);
    else if (name == "arith.divf")
      value = builder.create<LLVM::FDivOp>(op->getLoc(), a, b);
    else
      return op->emitError() << "no LLVM lowering for " << name;
    result.elts.push_back(value);
  }
  env[op->getResult(0)] = result;
  return success();
}

LogicalResult FuncLowerer::lowerMakeRange(tiny::ttg::MakeRangeOp op) {
  auto type = cast<RankedTensorType>(op.getResult().getType());
  FailureOr<int64_t> count = elementsPerThread(type);
  if (failed(count))
    return op.emitError() << "cannot compute elements per thread";
  if (op.getAxis() != 0)
    return op.emitError() << "chapter 5 only supports axis = 0";

  // GPUMin distributes consecutive elements: thread `tid` holds element
  // `r * tile + tid` for r = 0 .. elementsPerThread-1.  A range shorter than
  // one tile (chapter 7's `tl.arange(0, 8)`) has no room for all 32 threads,
  // so the extra threads repeat it: the element is `tid % extent`.  Such a
  // range only ever feeds `[:, None]` / `[None, :]`, where the consumer
  // recomputes it from the destination coordinates anyway.
  auto blocked = cast<tiny::ttg::BlockedAttr>(type.getEncoding());
  int64_t tile = blocked.getTileSize(0);
  int64_t extent = type.getShape()[0];

  Value thread = tid;
  if (extent < tile)
    thread = builder.create<LLVM::URemOp>(op.getLoc(), tid,
                                          constant(extent));

  LaneValue value;
  for (int64_t r = 0; r < *count; ++r) {
    Value base = constant(r * tile);
    value.elts.push_back(
        builder.create<LLVM::AddOp>(op.getLoc(), base, thread));
  }
  env[op.getResult()] = value;
  return success();
}

LogicalResult FuncLowerer::lowerExpandDims(tiny::ttg::ExpandDimsOp op) {
  auto resultType = cast<RankedTensorType>(op.getResult().getType());
  auto blocked = cast<tiny::ttg::BlockedAttr>(resultType.getEncoding());
  int64_t axis = op.getAxis();

  // A range is a function of its coordinate, so expanding it needs no
  // cross-thread movement at all: recompute `arange` at the *destination*
  // coordinates.  This is the same observation Triton's own lowering uses
  // for `arange(...)[:, None]`, and it keeps chapter 7 free of shuffles.
  auto range = op.getInput().getDefiningOp<tiny::ttg::MakeRangeOp>();
  if (!range)
    return op.emitError() << "chapter 7 only expands tl.arange results; a "
                             "computed 1-D tensor needs a layout conversion, "
                             "which chapter 7b adds";

  int64_t sourceDim = axis == 0 ? 1 : 0;
  int64_t count = tiny::layoutElementsPerThread(blocked, resultType.getShape());
  Value linear = threadIndexInLayout(blocked);

  LaneValue value;
  for (int64_t i = 0; i < count; ++i) {
    tiny::ElementPosition position = tiny::elementPosition(
        blocked, resultType.getShape(), i);
    value.elts.push_back(
        coordValue(resultType, position, sourceDim, linear));
  }
  env[op.getResult()] = value;
  return success();
}

LogicalResult FuncLowerer::lowerBroadcast(tiny::ttg::BroadcastOp op) {
  auto sourceType = cast<RankedTensorType>(op.getInput().getType());
  auto resultType = cast<RankedTensorType>(op.getResult().getType());
  auto sourceBlocked = cast<tiny::ttg::BlockedAttr>(sourceType.getEncoding());
  auto resultBlocked = cast<tiny::ttg::BlockedAttr>(resultType.getEncoding());

  LaneValue source = lookup(op.getInput());
  int64_t count =
      tiny::layoutElementsPerThread(resultBlocked, resultType.getShape());
  if (static_cast<int64_t>(source.elts.size()) == 0)
    return op.emitError() << "broadcast input has no lane values";

  // Growing a size-1 dimension only repeats registers a thread already
  // holds: both layouts keep the same number of elements per thread, because
  // the size-1 dimension already used its lanes as copies.  Map each
  // destination register back to the source register with the same repeat.
  LaneValue value;
  for (int64_t i = 0; i < count; ++i) {
    tiny::ElementPosition position =
        tiny::elementPosition(resultBlocked, resultType.getShape(), i);
    for (int64_t dim = 0; dim < resultType.getRank(); ++dim) {
      if (sourceType.getShape()[dim] == 1) {
        position.repeat[dim] = 0;
        position.within[dim] = 0;
      }
    }
    int64_t index =
        tiny::linearElementIndex(sourceBlocked, sourceType.getShape(), position);
    if (index >= static_cast<int64_t>(source.elts.size()))
      return op.emitError() << "broadcast maps a destination register outside "
                               "the source layout";
    value.elts.push_back(source.elts[index]);
  }
  env[op.getResult()] = value;
  return success();
}

LogicalResult FuncLowerer::lowerSplat(tiny::ttg::SplatOp op) {
  auto type = cast<RankedTensorType>(op.getResult().getType());
  FailureOr<int64_t> count = elementsPerThread(type);
  if (failed(count))
    return op.emitError() << "cannot compute elements per thread";

  Value scalar = lookup(op.getSrc()).elts.front();
  LaneValue value;
  value.elts.assign(*count, scalar);
  env[op.getResult()] = value;
  return success();
}

LogicalResult FuncLowerer::lowerAddPtr(tiny::ttg::AddPtrOp op) {
  LaneValue base = lookup(op.getPtr());
  LaneValue offset = lookup(op.getOffset());
  LaneValue result;
  for (size_t i = 0; i < base.elts.size(); ++i) {
    Value address = builder.create<LLVM::PtrToIntOp>(
        op.getLoc(), builder.getI32Type(), base.elts[i]);
    Value scaled = builder.create<LLVM::MulOp>(
        op.getLoc(), offset.elts[i], constant(4));
    Value advanced = builder.create<LLVM::AddOp>(op.getLoc(), address, scaled);
    result.elts.push_back(
        builder.create<LLVM::IntToPtrOp>(op.getLoc(), ptrTy, advanced));
  }
  env[op.getResult()] = result;
  return success();
}

LogicalResult FuncLowerer::lowerLoad(tiny::ttg::LoadOp op) {
  auto type = cast<RankedTensorType>(op.getResult().getType());
  LaneValue pointers = lookup(op.getPtr());
  LaneValue result;
  for (Value pointer : pointers.elts)
    result.elts.push_back(
        builder.create<LLVM::LoadOp>(op.getLoc(), type.getElementType(), pointer));
  env[op.getResult()] = result;
  return success();
}

LogicalResult FuncLowerer::lowerStore(tiny::ttg::StoreOp op) {
  LaneValue pointers = lookup(op.getPtr());
  LaneValue values = lookup(op.getValue());
  for (auto [pointer, value] : llvm::zip_equal(pointers.elts, values.elts))
    builder.create<LLVM::StoreOp>(op.getLoc(), value, pointer);
  return success();
}

LogicalResult FuncLowerer::lowerStoreScalar(tiny::ttg::StoreScalarOp op) {
  Value pointer = lookup(op.getPtr()).elts.front();
  Value value = lookup(op.getValue()).elts.front();
  Location loc = op.getLoc();

  // Every thread holds the same reduced scalar; only lane 0 should write it.
  // Keeping the store behind a branch also makes the memory effect match the
  // Triton semantics instead of relying on a benign write race.
  Value isLaneZero = builder.create<LLVM::ICmpOp>(
      loc, LLVM::ICmpPredicate::eq, tid, constant(0));
  Block *storeBlock = new Block();
  Block *continueBlock = new Block();
  Region *body = builder.getInsertionBlock()->getParent();
  body->getBlocks().push_back(storeBlock);
  body->getBlocks().push_back(continueBlock);

  builder.create<LLVM::CondBrOp>(loc, isLaneZero, storeBlock, continueBlock,
                                 ValueRange{});
  builder.setInsertionPointToStart(storeBlock);
  builder.create<LLVM::StoreOp>(loc, value, pointer);
  builder.create<LLVM::BrOp>(loc, continueBlock);

  builder.setInsertionPointToStart(continueBlock);
  return success();
}

LogicalResult FuncLowerer::lowerReduce(tiny::ttg::ReduceOp op) {
  auto type = dyn_cast<RankedTensorType>(op.getInput().getType());
  if (!type)
    return op.emitError() << "reduce expects a ranked tensor";
  if (op.getCombine() != "add")
    return op.emitError() << "chapter 6 only lowers combine = \"add\"";
  Type elementType = op.getResult().getType();
  if (!elementType.isF32() && !elementType.isInteger(32))
    return op.emitError() << "reduce supports f32 and i32 only";

  Location loc = op.getLoc();
  LaneValue source = lookup(op.getInput());
  if (source.elts.empty())
    return op.emitError() << "reduce input has no lane values";

  // 1. Every thread reduces its own registers first: 1024 elements over 32
  //    threads is 32 adds, not 32 shared round trips.
  Value partial = source.elts.front();
  for (size_t i = 1; i < source.elts.size(); ++i) {
    if (elementType.isF32())
      partial = builder.create<LLVM::FAddOp>(loc, partial, source.elts[i]);
    else
      partial = builder.create<LLVM::AddOp>(loc, partial, source.elts[i]);
  }

  // 2. One shared slot per thread.  The number of slots is the number of
  //    threads in the layout, not the number of elements each thread holds:
  //    a 512-element reduce still has 32 partials even though every thread
  //    only contributes 16 values.  `llvm.gpumin.shared` is the block's
  //    shared-window base, so the offset is ours to choose.
  FailureOr<int64_t> threads = threadsInLayout(type);
  if (failed(threads))
    return op.emitError() << "cannot compute the number of threads in the "
                             "reduction layout";
  Value base = intrinsic("llvm.gpumin.shared");
  Value window = constant(sharedOffset);
  Value tidBytes = builder.create<LLVM::MulOp>(loc, tid, constant(4));
  Value slotOffset = builder.create<LLVM::AddOp>(loc, window, tidBytes);
  Value slotAddress = builder.create<LLVM::AddOp>(loc, base, slotOffset);
  Value slot = builder.create<LLVM::IntToPtrOp>(loc, sharedPtrTy, slotAddress);
  builder.create<LLVM::StoreOp>(loc, partial, slot);

  // 3. The barrier makes every slot visible to every thread, so each thread
  //    can compute the whole sum.  No second barrier is needed: after this
  //    point nothing writes shared memory.
  builder.create<LLVM::CallIntrinsicOp>(
      loc, builder.getStringAttr("llvm.gpumin.barrier"), ValueRange{});

  Value sum;
  for (int64_t i = 0; i < *threads; ++i) {
    Value offset = constant(sharedOffset + 4 * i);
    Value address = builder.create<LLVM::AddOp>(loc, base, offset);
    Value pointer = builder.create<LLVM::IntToPtrOp>(loc, sharedPtrTy, address);
    Value value =
        builder.create<LLVM::LoadOp>(loc, elementType, pointer);
    if (!sum)
      sum = value;
    else if (elementType.isF32())
      sum = builder.create<LLVM::FAddOp>(loc, sum, value);
    else
      sum = builder.create<LLVM::AddOp>(loc, sum, value);
  }

  sharedOffset += 4 * *threads;
  env[op.getResult()] = LaneValue{{sum}};
  return success();
}

Value FuncLowerer::threadIndexInLayout(const tiny::ttg::BlockedAttr &blocked) {
  int64_t threads = tiny::layoutThreadsInLayout(blocked);
  if (threads == target.threadsPerBlock())
    return tid;
  return builder.create<LLVM::URemOp>(source.getLoc(), tid,
                                      constant(threads));
}

Value FuncLowerer::coordValue(RankedTensorType type,
                              const tiny::ElementPosition &position,
                              int64_t dim, Value linear) {
  auto blocked = cast<tiny::ttg::BlockedAttr>(type.getEncoding());
  int64_t extent = type.getShape()[dim];
  // A dimension of extent 1 has no threads of its own: every thread holds
  // the same value, so the coordinate is 0 no matter which lane asks.
  if (extent == 1)
    return constant(0);

  Location loc = source.getLoc();
  int64_t stride = tiny::layoutThreadStride(blocked, dim);
  int64_t threads = tiny::layoutThreads(blocked, dim);
  int64_t perThread = blocked.getSizePerThread()[dim];
  int64_t tile = blocked.getTileSize(dim);

  // `linear` is already clamped to the layout's threads, so the modulo is
  // only needed when this dimension does not span the whole layout.
  int64_t layoutThreads = tiny::layoutThreadsInLayout(blocked);
  Value coord = linear;
  if (stride != 1)
    coord = builder.create<LLVM::UDivOp>(loc, coord, constant(stride));
  if (threads != 1 && stride * threads != layoutThreads)
    coord = builder.create<LLVM::URemOp>(loc, coord, constant(threads));
  if (perThread != 1)
    coord = builder.create<LLVM::MulOp>(loc, coord, constant(perThread));
  if (position.within[dim] != 0)
    coord = builder.create<LLVM::AddOp>(loc, coord,
                                        constant(position.within[dim]));
  if (position.repeat[dim] != 0)
    coord = builder.create<LLVM::AddOp>(
        loc, coord, constant(position.repeat[dim] * tile));

  // A tensor shorter than one tile wraps: `tid % extent` repeats the small
  // range over the block.  Chapter 7 only produces this for `tl.arange`.
  if (extent < tile)
    coord = builder.create<LLVM::URemOp>(loc, coord, constant(extent));
  return coord;
}

LogicalResult FuncLowerer::lowerOp(Operation *op) {
  if (auto constant = dyn_cast<arith::ConstantOp>(op))
    return lowerConstant(constant);

  if (auto range = dyn_cast<tiny::ttg::MakeRangeOp>(op))
    return lowerMakeRange(range);
  if (auto splat = dyn_cast<tiny::ttg::SplatOp>(op))
    return lowerSplat(splat);
  if (auto addptr = dyn_cast<tiny::ttg::AddPtrOp>(op))
    return lowerAddPtr(addptr);
  if (auto load = dyn_cast<tiny::ttg::LoadOp>(op))
    return lowerLoad(load);
  if (auto store = dyn_cast<tiny::ttg::StoreOp>(op))
    return lowerStore(store);
  if (auto store = dyn_cast<tiny::ttg::StoreScalarOp>(op))
    return lowerStoreScalar(store);
  if (auto reduce = dyn_cast<tiny::ttg::ReduceOp>(op))
    return lowerReduce(reduce);
  if (auto expand = dyn_cast<tiny::ttg::ExpandDimsOp>(op))
    return lowerExpandDims(expand);
  if (auto broadcast = dyn_cast<tiny::ttg::BroadcastOp>(op))
    return lowerBroadcast(broadcast);
  if (auto pid = dyn_cast<tiny::ttg::GetProgramIdOp>(op)) {
    if (pid.getAxis() != 0)
      return pid.emitError() << "chapter 5 only supports axis = 0";
    env[pid.getResult()] = LaneValue{{intrinsic("llvm.gpumin.blockidx")}};
    return success();
  }

  StringRef name = op->getName().getStringRef();
  if (name.starts_with("arith.") && op->getNumOperands() == 2 &&
      op->getNumResults() == 1)
    return lowerBinary(op, name);

  if (auto ret = dyn_cast<func::ReturnOp>(op)) {
    if (ret.getNumOperands() != 0)
      return ret.emitError() << "tiny kernels return void";
    builder.create<LLVM::ReturnOp>(ret.getLoc(), ValueRange{});
    return success();
  }

  return op->emitError() << "no LLVM lowering for this operation in chapter 5";
}

LogicalResult FuncLowerer::run() {
  MLIRContext *context = source.getContext();
  ptrTy = LLVM::LLVMPointerType::get(context, 1);
  sharedPtrTy = LLVM::LLVMPointerType::get(context, 2);

  // Kernel ABI: no formal parameters, void return.
  auto fnType = LLVM::LLVMFunctionType::get(LLVM::LLVMVoidType::get(context), {});
  builder.setInsertionPoint(source);
  auto kernel = builder.create<LLVM::LLVMFuncOp>(
      source.getLoc(), source.getSymName(), fnType, LLVM::Linkage::External,
      /*dsoLocal=*/false, LLVM::CConv::C, /*comdat=*/SymbolRefAttr{},
      /*attrs=*/ArrayRef<NamedAttribute>{}, /*argAttrs=*/ArrayRef<DictionaryAttr>{},
      /*functionEntryCount=*/std::nullopt);
  kernel.setPassthroughAttr(builder.getArrayAttr(
      {builder.getStringAttr("gpumin_kernel")}));

  Block *body = kernel.addEntryBlock(builder);
  builder.setInsertionPointToStart(body);
  tid = intrinsic("llvm.gpumin.tid");

  for (auto [index, argument] : llvm::enumerate(source.getArguments())) {
    FailureOr<Value> value = argumentValue(index, argument.getType());
    if (failed(value))
      return failure();
    env[argument] = LaneValue{{*value}};
  }

  for (Operation &op : source.getBody().front()) {
    if (failed(lowerOp(&op)))
      return failure();
  }

  source.erase();
  return success();
}

struct TinyTTGToLLVMPass
    : public PassWrapper<TinyTTGToLLVMPass, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(TinyTTGToLLVMPass)

  StringRef getArgument() const final { return "tiny-convert-ttg-to-llvm"; }
  StringRef getDescription() const final {
    return "lower tinyttg kernels to the LLVM dialect";
  }

  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<LLVM::LLVMDialect, func::FuncDialect>();
  }

  void runOnOperation() override {
    ModuleOp module = getOperation();

    SmallVector<func::FuncOp> functions;
    module.walk([&](func::FuncOp func) { functions.push_back(func); });

    for (func::FuncOp func : functions) {
      if (func.isExternal())
        continue;
      FuncLowerer lowerer(func, module);
      if (failed(lowerer.run())) {
        signalPassFailure();
        return;
      }
    }

    OpBuilder builder(module.getContext());
    module->setAttr("llvm.target_triple",
                    builder.getStringAttr("gpumin-unknown-elf"));
  }
};

} // namespace

std::unique_ptr<Pass> tiny::createTinyTTGToLLVMPass() {
  return std::make_unique<TinyTTGToLLVMPass>();
}

void tiny::registerTinyTTGToLLVMPass() {
  PassRegistration<TinyTTGToLLVMPass>();
}
