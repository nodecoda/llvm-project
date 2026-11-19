//===- TinyTTToTinyTTG.cpp - TTIR -> TTGIR ---------------------*- C++ -*-===//
//
// Chapter 4 gives every tensor a thread mapping.  Two things change and
// nothing else: the element type of a tensor gains an encoding, and the op
// moves from the `tinytt` dialect to `tinyttg`.
//
// This pass rewrites types in place instead of running a full dialect
// conversion.  The SSA graph is untouched: no value is created or destroyed,
// only its type changes, so a conversion framework would add machinery
// without adding semantics.  Chapter 5, which really does change the graph,
// switches to a TypeConverter.
//
//===----------------------------------------------------------------------===//

#include "tiny/Conversion/TinyTTToTinyTTG.h"

#include "tiny/Conversion/TargetInfo.h"
#include "tiny/Dialect/TinyTT/TinyTTOps.h"
#include "tiny/Dialect/TinyTTG/TinyTTGOps.h"

#include "llvm/ADT/STLExtras.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/Pass/Pass.h"

using namespace mlir;

namespace {

/// Replace a tensor type that has no encoding with the default blocked
/// layout.  Non-tensor types and already-encoded tensors pass through.
Type convertTensorType(Type type, Location loc, const tiny::TargetInfo &target,
                       bool &failed) {
  auto tensor = dyn_cast<RankedTensorType>(type);
  if (!tensor || tensor.getEncoding())
    return type;

  auto blocked = target.getDefaultBlocked(type.getContext(), tensor.getRank());
  if (!blocked) {
    emitError(loc) << "tiny only assigns layouts to rank 1 and 2 tensors, got "
                   << "rank " << tensor.getRank();
    failed = true;
    return type;
  }

  for (int64_t dim = 0; dim < tensor.getRank(); ++dim) {
    int64_t extent = tensor.getShape()[dim];
    int64_t tile = blocked.getTileSize(dim);
    if (extent < 0 || extent % tile != 0) {
      emitError(loc) << "tensor dimension " << dim << " of size " << extent
                     << " is not divisible by the " << tile
                     << " threads that one tile covers";
      failed = true;
      return type;
    }
  }
  return RankedTensorType::get(tensor.getShape(), tensor.getElementType(),
                               blocked);
}

/// Rebuild one `tinytt` operation as its `tinyttg` twin.  Result types are
/// already converted when this runs, so only the op itself is replaced.
LogicalResult rewriteOp(Operation *op) {
  OpBuilder builder(op);
  Location loc = op->getLoc();

  auto finish = [&](Operation *newOp) {
    if (op->getNumResults() != 0)
      op->replaceAllUsesWith(newOp->getResults());
    op->erase();
  };

  if (auto old = dyn_cast<tiny::tt::GetProgramIdOp>(op)) {
    auto newOp = builder.create<tiny::ttg::GetProgramIdOp>(
        loc, old.getResult().getType(), old.getAxisAttr());
    finish(newOp);
    return success();
  }
  if (auto old = dyn_cast<tiny::tt::MakeRangeOp>(op)) {
    auto newOp = builder.create<tiny::ttg::MakeRangeOp>(
        loc, old.getResult().getType(), old.getAxisAttr());
    finish(newOp);
    return success();
  }
  if (auto old = dyn_cast<tiny::tt::SplatOp>(op)) {
    auto newOp = builder.create<tiny::ttg::SplatOp>(
        loc, old.getResult().getType(), old.getSrc());
    finish(newOp);
    return success();
  }
  if (auto old = dyn_cast<tiny::tt::AddPtrOp>(op)) {
    auto newOp = builder.create<tiny::ttg::AddPtrOp>(
        loc, old.getResult().getType(), old.getPtr(), old.getOffset());
    finish(newOp);
    return success();
  }
  if (auto old = dyn_cast<tiny::tt::LoadOp>(op)) {
    auto newOp = builder.create<tiny::ttg::LoadOp>(
        loc, old.getResult().getType(), old.getPtr());
    finish(newOp);
    return success();
  }
  if (auto old = dyn_cast<tiny::tt::MaskedLoadOp>(op)) {
    auto newOp = builder.create<tiny::ttg::MaskedLoadOp>(
        loc, old.getResult().getType(), old.getPtr(), old.getMask(),
        old.getOther());
    finish(newOp);
    return success();
  }
  if (auto old = dyn_cast<tiny::tt::StoreOp>(op)) {
    auto newOp = builder.create<tiny::ttg::StoreOp>(loc, old.getPtr(),
                                                    old.getValue());
    finish(newOp);
    return success();
  }
  if (auto old = dyn_cast<tiny::tt::MaskedStoreOp>(op)) {
    auto newOp = builder.create<tiny::ttg::MaskedStoreOp>(
        loc, old.getPtr(), old.getValue(), old.getMask());
    finish(newOp);
    return success();
  }
  if (auto old = dyn_cast<tiny::tt::ReduceOp>(op)) {
    auto newOp = builder.create<tiny::ttg::ReduceOp>(
        loc, old.getResult().getType(), old.getInput(), old.getAxisAttr(),
        old.getCombineAttr());
    finish(newOp);
    return success();
  }
  if (auto old = dyn_cast<tiny::tt::TransOp>(op)) {
    auto newOp = builder.create<tiny::ttg::TransOp>(
        loc, old.getResult().getType(), old.getInput(), old.getOrderAttr());
    finish(newOp);
    return success();
  }
  if (auto old = dyn_cast<tiny::tt::BroadcastOp>(op)) {
    auto newOp = builder.create<tiny::ttg::BroadcastOp>(
        loc, old.getResult().getType(), old.getInput());
    finish(newOp);
    return success();
  }
  if (auto old = dyn_cast<tiny::tt::ExpandDimsOp>(op)) {
    auto newOp = builder.create<tiny::ttg::ExpandDimsOp>(
        loc, old.getResult().getType(), old.getInput(), old.getAxisAttr());
    finish(newOp);
    return success();
  }

  return op->emitOpError("has no tinyttg equivalent yet");
}

struct TinyTTToTinyTTGPass
    : public PassWrapper<TinyTTToTinyTTGPass, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(TinyTTToTinyTTGPass)

  StringRef getArgument() const final { return "tiny-convert-tt-to-ttg"; }
  StringRef getDescription() const final {
    return "assign GPUMin layouts to tinytt and produce tinyttg";
  }

  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<tiny::ttg::TinyTTGDialect, func::FuncDialect>();
  }

  void runOnOperation() override {
    ModuleOp module = getOperation();
    MLIRContext *context = &getContext();
    tiny::TargetInfo target;
    bool hadError = false;

    // 1. Function signatures and their entry-block arguments.
    module.walk([&](func::FuncOp func) {
      FunctionType fn = func.getFunctionType();
      SmallVector<Type> inputs, results;
      inputs.reserve(fn.getNumInputs());
      results.reserve(fn.getNumResults());
      for (Type type : fn.getInputs())
        inputs.push_back(convertTensorType(type, func.getLoc(), target, hadError));
      for (Type type : fn.getResults())
        results.push_back(
            convertTensorType(type, func.getLoc(), target, hadError));

      func.setType(FunctionType::get(context, inputs, results));
      if (!func.isExternal())
        for (auto [argument, type] :
             llvm::zip_equal(func.getBody().front().getArguments(), inputs))
          argument.setType(type);
    });

    // 2. Every other tensor result.  Changing the type in place is enough:
    //    all uses observe it, and no SSA edge changes.
    module.walk([&](Operation *op) {
      for (Value result : op->getResults()) {
        Type converted = convertTensorType(result.getType(), op->getLoc(),
                                           target, hadError);
        if (converted != result.getType())
          result.setType(converted);
      }
    });

    if (hadError) {
      signalPassFailure();
      return;
    }

    // 3. Move the twelve operations to the tinyttg dialect.
    SmallVector<Operation *> tinyOps;
    module.walk([&](Operation *op) {
      if (op->getName().getDialectNamespace() == "tinytt")
        tinyOps.push_back(op);
    });
    for (Operation *op : tinyOps)
      if (mlir::failed(rewriteOp(op)))
        hadError = true;
    if (hadError) {
      signalPassFailure();
      return;
    }

    // 4. Record the target on the module, so TTGIR is self-describing.
    OpBuilder builder(context);
    module->setAttr("tinyttg.num_warps",
                    builder.getI32IntegerAttr(target.warpsPerCTA));
  }
};

} // namespace

std::unique_ptr<Pass> tiny::createTinyTTToTinyTTGPass() {
  return std::make_unique<TinyTTToTinyTTGPass>();
}

void tiny::registerTinyTTToTinyTTGPass() {
  PassRegistration<TinyTTToTinyTTGPass>();
}
