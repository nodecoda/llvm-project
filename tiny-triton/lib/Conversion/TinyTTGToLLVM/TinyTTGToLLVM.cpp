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

/// Elements one thread holds for `type`, assuming sizePerThread is 1 (the
/// chapter 3 contract).
FailureOr<int64_t> elementsPerThread(RankedTensorType type) {
  auto blocked = dyn_cast<tiny::ttg::BlockedAttr>(type.getEncoding());
  if (!blocked)
    return failure();

  int64_t count = 1;
  for (int64_t dim = 0; dim < type.getRank(); ++dim) {
    int64_t tile = blocked.getTileSize(dim);
    if (tile <= 0 || type.getShape()[dim] % tile != 0)
      return failure();
    count *= type.getShape()[dim] / tile;
  }
  for (int64_t size : blocked.getSizePerThread())
    count *= size;
  return count;
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

  LaneValue lookup(Value value) const { return env.lookup(value); }

  func::FuncOp source;
  ModuleOp module;
  OpBuilder builder;
  DenseMap<Value, LaneValue> env;
  Value tid;
  Type ptrTy;
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
  // `r * threads + tid` for r = 0 .. elementsPerThread-1.
  int64_t threads = 1;
  auto blocked = cast<tiny::ttg::BlockedAttr>(type.getEncoding());
  for (int64_t dim = 0; dim < type.getRank(); ++dim)
    threads *= blocked.getTileSize(dim);

  LaneValue value;
  for (int64_t r = 0; r < *count; ++r) {
    Value base = constant(r * threads);
    value.elts.push_back(builder.create<LLVM::AddOp>(op.getLoc(), base, tid));
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
