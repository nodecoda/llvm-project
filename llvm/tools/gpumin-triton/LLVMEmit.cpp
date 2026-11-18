//===-- LLVMEmit.cpp - TTIR-min to GPUMin LLVM IR -------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "LLVMEmit.h"

#include "llvm/ADT/APFloat.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"
#include <map>
#include "llvm/TargetParser/Triple.h"

using namespace llvm;

namespace gpumintriton {

namespace {

using LlvmValue = llvm::Value;

constexpr unsigned ThreadsPerBlock = 32;
// The 4x8 thread grid is the layout contract for 2D tensors: warpid selects a
// row group and laneid selects a column, exactly like sr.warpid/sr.laneid.
constexpr unsigned LanesPerWarp = 8;
constexpr unsigned WarpsPerBlock = ThreadsPerBlock / LanesPerWarp;
constexpr unsigned MaxElementsPerThread = 8;

struct Lowered {
  std::vector<LlvmValue *> Elts;
  bool IsTensor = false;
  // Chapter 34: set only for an alloc_shared handle.
  llvm::GlobalVariable *Shared = nullptr;
};

class FuncEmitter {
public:
  FuncEmitter(llvm::Module &M, const Function &TF)
      : M(M), TF(TF), Ctx(M.getContext()), B(Ctx) {}
  Error emit();

private:
  Expected<unsigned> elementsPerThread(Type Ty) const;
  Expected<std::pair<int64_t, int64_t>> threadLayout(Type Ty) const;
  Expected<std::pair<int64_t, int64_t>> localCoord(Type Ty, unsigned I) const;
  LlvmValue *getTid();
  LlvmValue *rowGroup();
  LlvmValue *laneId();
  LlvmValue *coordValue(Type Ty, unsigned I, bool WantRow);
  llvm::Type *llvmType(Type Ty);
  LlvmValue *elementAt(const Lowered &V, unsigned R) const;
  LlvmValue *maskedLoad(LlvmValue *Ptr, LlvmValue *Mask, LlvmValue *Other,
                        llvm::Type *ElemTy);
  LlvmValue *combineReduction(ReduceKind Kind, Type ElemTy, LlvmValue *A,
                              LlvmValue *Rhs);
  Error emitReduce(const Op &O);
  Error emitReduce2D(const Op &O);
  Error emitSharedStore(const Op &O);
  Lowered emitSharedSlice(const Op &O);
  Error emitDot(const Op &O);
  llvm::GlobalVariable *createSharedWindow(Type TensorTy);
  void storeSharedTile(llvm::GlobalVariable *Window, Type SrcTy,
                       llvm::ArrayRef<LlvmValue *> Elts);
  LlvmValue *loadSharedElem(llvm::GlobalVariable *Window, int64_t Stride,
                            LlvmValue *Row, LlvmValue *Col, llvm::Type *ElemTy);
  void barrierIfSharedDirty();
  void maskedStore(LlvmValue *Ptr, LlvmValue *Stored, LlvmValue *Mask);
  Error emitOp(const Op &O);
  Error emitOps(llvm::ArrayRef<Op> Ops);
  Error emitPrologue();

  llvm::Module &M;
  const Function &TF;
  LLVMContext &Ctx;
  IRBuilder<> B;
  llvm::Function *LF = nullptr;
  std::map<const gpumintriton::Value *, Lowered> Env;
  std::map<const gpumintriton::Value *, const Op *> Defs;
  LlvmValue *TidCache = nullptr;
  unsigned ReduceCounter = 0;
  unsigned TransCounter = 0;
  unsigned SharedCounter = 0;
  bool SharedDirty = false;
};

Expected<unsigned> FuncEmitter::elementsPerThread(Type Ty) const {
  if (!Ty.isTensor())
    return 1;
  llvm::ArrayRef<int64_t> Shape = Ty.getShape();
  if (Shape.size() == 1) {
    int64_t N = Shape[0];
    if (N <= 0 || N % ThreadsPerBlock != 0)
      return createStringError(inconvertibleErrorCode(),
                               "tensor size must be a positive multiple of %u, "
                               "got %lld",
                               ThreadsPerBlock, static_cast<long long>(N));
    return static_cast<unsigned>(N / ThreadsPerBlock);
  }
  if (Shape.size() != 2)
    return createStringError(inconvertibleErrorCode(),
                             "only 1D and 2D tensors are supported");
  Expected<std::pair<int64_t, int64_t>> LayoutOrErr = threadLayout(Ty);
  if (!LayoutOrErr)
    return LayoutOrErr.takeError();
  return static_cast<unsigned>(LayoutOrErr->first * LayoutOrErr->second);
}

Expected<std::pair<int64_t, int64_t>> FuncEmitter::threadLayout(Type Ty) const {
  llvm::ArrayRef<int64_t> Shape = Ty.getShape();
  if (Shape.size() != 2)
    return createStringError(inconvertibleErrorCode(),
                             "threadLayout needs a 2D tensor");
  int64_t R = Shape[0], C = Shape[1];
  int64_t Rows = 1, Cols = 1;
  if (R != 1) {
    if (R <= 0 || R % WarpsPerBlock != 0)
      return createStringError(inconvertibleErrorCode(),
                               "2D tensor rows must be 1 or a multiple of %u, "
                               "got %lld",
                               WarpsPerBlock, static_cast<long long>(R));
    Rows = R / WarpsPerBlock;
  }
  if (C != 1) {
    if (C <= 0 || C % LanesPerWarp != 0)
      return createStringError(inconvertibleErrorCode(),
                               "2D tensor columns must be 1 or a multiple of "
                               "%u, got %lld",
                               LanesPerWarp, static_cast<long long>(C));
    Cols = C / LanesPerWarp;
  }
  if (Rows * Cols > MaxElementsPerThread)
    return createStringError(inconvertibleErrorCode(),
                             "2D tensor needs at most %u elements per thread, "
                             "got %lld",
                             MaxElementsPerThread,
                             static_cast<long long>(Rows * Cols));
  return std::make_pair(Rows, Cols);
}

/// Thread-local (row, column) inside the block a thread owns.  The caller adds
/// warpid*rowsPerThread / laneid*colsPerThread; dimensions of size 1 stay 0.
Expected<std::pair<int64_t, int64_t>>
FuncEmitter::localCoord(Type Ty, unsigned I) const {
  Expected<std::pair<int64_t, int64_t>> LayoutOrErr = threadLayout(Ty);
  if (!LayoutOrErr)
    return LayoutOrErr.takeError();
  int64_t Rows = LayoutOrErr->first, Cols = LayoutOrErr->second;
  if (I >= static_cast<unsigned>(Rows * Cols))
    return createStringError(inconvertibleErrorCode(),
                             "element index %u is outside the thread's block",
                             I);
  return std::make_pair(static_cast<int64_t>(I) / Cols,
                        static_cast<int64_t>(I) % Cols);
}

LlvmValue *FuncEmitter::getTid() {
  if (!TidCache) {
    FunctionCallee TidFn =
        M.getOrInsertFunction("llvm.gpumin.tid", B.getInt32Ty());
    TidCache = B.CreateCall(TidFn);
  }
  return TidCache;
}

LlvmValue *FuncEmitter::rowGroup() {
  return B.CreateLShr(getTid(), B.getInt32(3)); // tid / lanesPerWarp
}

LlvmValue *FuncEmitter::laneId() {
  return B.CreateAnd(getTid(), B.getInt32(LanesPerWarp - 1));
}

/// Global row (WantRow) or column for element I of a 2D tensor.
LlvmValue *FuncEmitter::coordValue(Type Ty, unsigned I, bool WantRow) {
  llvm::ArrayRef<int64_t> Shape = Ty.getShape();
  Expected<std::pair<int64_t, int64_t>> LayoutOrErr = threadLayout(Ty);
  assert(LayoutOrErr && "bad 2D layout");
  int64_t Rows = LayoutOrErr->first, Cols = LayoutOrErr->second;
  Expected<std::pair<int64_t, int64_t>> LocalOrErr = localCoord(Ty, I);
  assert(LocalOrErr && "bad element index");
  int64_t LocalRow = LocalOrErr->first, LocalCol = LocalOrErr->second;
  if (WantRow) {
    if (Shape[0] == 1)
      return B.getInt32(0);
    LlvmValue *Base = B.CreateMul(rowGroup(), B.getInt32(Rows));
    return B.CreateAdd(Base, B.getInt32(LocalRow));
  }
  if (Shape[1] == 1)
    return B.getInt32(0);
  LlvmValue *Base = B.CreateMul(laneId(), B.getInt32(Cols));
  return B.CreateAdd(Base, B.getInt32(LocalCol));
}

llvm::Type *FuncEmitter::llvmType(Type Ty) {
  switch (Ty.getKind()) {
  case Type::Kind::I1:
    return B.getInt1Ty();
  case Type::Kind::I32:
    return B.getInt32Ty();
  case Type::Kind::F32:
    return B.getFloatTy();
  case Type::Kind::Ptr:
    return PointerType::get(Ctx, 1);
  case Type::Kind::Tensor:
    return llvmType(Ty.getElementType());
  }
  llvm_unreachable("unknown TTIR type");
}

LlvmValue *FuncEmitter::elementAt(const Lowered &V, unsigned R) const {
  return V.IsTensor ? V.Elts[R] : V.Elts[0];
}

LlvmValue *FuncEmitter::maskedLoad(LlvmValue *Ptr, LlvmValue *Mask,
                                   LlvmValue *Other, llvm::Type *ElemTy) {
  if (!Mask)
    return B.CreateLoad(ElemTy, Ptr);
  BasicBlock *Before = B.GetInsertBlock();
  BasicBlock *LoadBB = BasicBlock::Create(Ctx, "load", LF);
  BasicBlock *MergeBB = BasicBlock::Create(Ctx, "merge", LF);
  B.CreateCondBr(Mask, LoadBB, MergeBB);
  B.SetInsertPoint(LoadBB);
  LlvmValue *Loaded = B.CreateLoad(ElemTy, Ptr);
  B.CreateBr(MergeBB);
  B.SetInsertPoint(MergeBB);
  PHINode *Phi = B.CreatePHI(ElemTy, 2);
  Phi->addIncoming(Loaded, LoadBB);
  Phi->addIncoming(Other ? Other : Constant::getNullValue(ElemTy), Before);
  return Phi;
}

void FuncEmitter::maskedStore(LlvmValue *Ptr, LlvmValue *Stored, LlvmValue *Mask) {
  if (!Mask) {
    B.CreateStore(Stored, Ptr);
    return;
  }
  BasicBlock *StoreBB = BasicBlock::Create(Ctx, "store", LF);
  BasicBlock *ContBB = BasicBlock::Create(Ctx, "cont", LF);
  B.CreateCondBr(Mask, StoreBB, ContBB);
  B.SetInsertPoint(StoreBB);
  B.CreateStore(Stored, Ptr);
  B.CreateBr(ContBB);
  B.SetInsertPoint(ContBB);
}

LlvmValue *FuncEmitter::combineReduction(ReduceKind Kind, Type ElemTy,
                                         LlvmValue *A, LlvmValue *Rhs) {
  if (Kind == ReduceKind::Sum)
    return ElemTy.isF32() ? B.CreateFAdd(A, Rhs) : B.CreateAdd(A, Rhs);
  // Chapter 28 enables f32 max: ch27 gave f32 SELP a pattern, so the ordered
  // compare plus select lowers without a bitcast.
  LlvmValue *Greater =
      ElemTy.isF32() ? B.CreateFCmpOGT(Rhs, A) : B.CreateICmpSGT(Rhs, A);
  return B.CreateSelect(Greater, Rhs, A);
}

void FuncEmitter::barrierIfSharedDirty() {
  if (!SharedDirty)
    return;
  FunctionCallee BarrierFn =
      M.getOrInsertFunction("llvm.gpumin.barrier", B.getVoidTy());
  B.CreateCall(BarrierFn);
  SharedDirty = false;
}

llvm::GlobalVariable *FuncEmitter::createSharedWindow(Type TensorTy) {
  llvm::ArrayRef<int64_t> Shape = TensorTy.getShape();
  llvm::Type *ElemTy = llvmType(TensorTy.getElementType());
  auto *SlotTy =
      ArrayType::get(ElemTy, static_cast<uint64_t>(Shape[0] * Shape[1]));
  std::string Name =
      ("__gpumin_sh_" + TF.getName() + "_" + Twine(SharedCounter++)).str();
  auto *Window =
      new GlobalVariable(M, SlotTy, false, GlobalValue::InternalLinkage,
                         Constant::getNullValue(SlotTy), Name, nullptr,
                         GlobalValue::NotThreadLocal, 2);
  Window->setAlignment(Align(4));
  return Window;
}

void FuncEmitter::storeSharedTile(llvm::GlobalVariable *Window, Type SrcTy,
                                  llvm::ArrayRef<LlvmValue *> Elts) {
  int64_t Cols = SrcTy.getShape()[1];
  auto *SlotTy = cast<ArrayType>(Window->getValueType());
  for (unsigned I = 0; I < Elts.size(); ++I) {
    LlvmValue *Row = coordValue(SrcTy, I, true);
    LlvmValue *Col = coordValue(SrcTy, I, false);
    LlvmValue *Index = B.CreateAdd(B.CreateMul(Row, B.getInt32(Cols)), Col);
    LlvmValue *Slot = B.CreateGEP(SlotTy, Window, {B.getInt32(0), Index});
    B.CreateStore(Elts[I], Slot);
  }
  SharedDirty = true;
}

LlvmValue *FuncEmitter::loadSharedElem(llvm::GlobalVariable *Window,
                                       int64_t Stride, LlvmValue *Row,
                                       LlvmValue *Col, llvm::Type *ElemTy) {
  auto *SlotTy = cast<ArrayType>(Window->getValueType());
  LlvmValue *Index = B.CreateAdd(B.CreateMul(Row, B.getInt32(Stride)), Col);
  LlvmValue *Slot = B.CreateGEP(SlotTy, Window, {B.getInt32(0), Index});
  return B.CreateLoad(ElemTy, Slot);
}

Error FuncEmitter::emitSharedStore(const Op &O) {
  auto It = Env.find(O.Operands[0]);
  if (It == Env.end() || !It->second.Shared)
    return createStringError(inconvertibleErrorCode(),
                             "shared_store source is not a shared handle");
  const Lowered &Src = Env.at(O.Operands[1]);
  storeSharedTile(It->second.Shared, O.Operands[1]->getType(), Src.Elts);
  return Error::success();
}

Lowered FuncEmitter::emitSharedSlice(const Op &O) {
  barrierIfSharedDirty();
  llvm::GlobalVariable *Window = Env.at(O.Operands[0]).Shared;
  Type HandleTy = O.Operands[0]->getType();
  int64_t Cols = HandleTy.getShape()[1];
  Type ResTy = O.Result->getType();
  llvm::Type *ElemTy = llvmType(ResTy.getElementType());
  bool IsCol = O.Kind == OpKind::SharedCol;
  Expected<unsigned> NumEltsOrErr = elementsPerThread(ResTy);
  if (!NumEltsOrErr) {
    consumeError(NumEltsOrErr.takeError());
    llvm_unreachable("shared slice shape is validated by the frontend");
  }
  unsigned NumElts = *NumEltsOrErr;

  Lowered V;
  V.IsTensor = true;
  for (unsigned I = 0; I < NumElts; ++I) {
    LlvmValue *Row = IsCol ? coordValue(ResTy, I, true)
                           : B.getInt32(static_cast<uint32_t>(O.Imm));
    LlvmValue *Col = IsCol ? B.getInt32(static_cast<uint32_t>(O.Imm))
                           : coordValue(ResTy, I, false);
    V.Elts.push_back(loadSharedElem(Window, Cols, Row, Col, ElemTy));
  }
  return V;
}

Error FuncEmitter::emitDot(const Op &O) {
  const Lowered &ATile = Env.at(O.Operands[0]);
  const Lowered &BTile = Env.at(O.Operands[1]);
  const Lowered &Acc = Env.at(O.Operands[2]);
  Type ATy = O.Operands[0]->getType();
  Type BTy = O.Operands[1]->getType();
  Type ResTy = O.Result->getType();
  llvm::ArrayRef<int64_t> AS = ATy.getShape();
  llvm::ArrayRef<int64_t> BS = BTy.getShape();
  int64_t K = AS[1], N = BS[1];

  // The dot is staging plus a rank-1 update, exactly like chapter 34's
  // hand-written loop.  The difference is where the loop lives: here it is
  // inside the backend, so the TTIR stays one operation.
  llvm::GlobalVariable *WA = createSharedWindow(ATy);
  storeSharedTile(WA, ATy, ATile.Elts);
  llvm::GlobalVariable *WB = createSharedWindow(BTy);
  storeSharedTile(WB, BTy, BTile.Elts);
  barrierIfSharedDirty();

  Expected<unsigned> CountOrErr = elementsPerThread(ResTy);
  if (!CountOrErr)
    return CountOrErr.takeError();
  llvm::Type *ElemTy = llvmType(ResTy.getElementType());
  Lowered V;
  V.IsTensor = true;
  for (unsigned I = 0; I < *CountOrErr; ++I) {
    LlvmValue *Row = coordValue(ResTy, I, true);
    LlvmValue *Col = coordValue(ResTy, I, false);
    LlvmValue *Sum = elementAt(Acc, I);
    for (int64_t KK = 0; KK < K; ++KK) {
      LlvmValue *AV = loadSharedElem(WA, K, Row, B.getInt32(KK), ElemTy);
      LlvmValue *BV = loadSharedElem(WB, N, B.getInt32(KK), Col, ElemTy);
      Sum = B.CreateFAdd(Sum, B.CreateFMul(AV, BV));
    }
    V.Elts.push_back(Sum);
  }
  Env[O.Result] = std::move(V);
  return Error::success();
}

Error FuncEmitter::emitReduce(const Op &O) {
  if (O.Operands[0]->getType().getShape().size() == 2)
    return emitReduce2D(O);

  const Lowered &Src = Env.at(O.Operands[0]);
  Type ResultTy = O.Result->getType();
  llvm::Type *ElemTy = llvmType(ResultTy);

  // Every thread reduces its own elements first, so the shared round trip
  // only has one value per thread.
  LlvmValue *Partial = Src.Elts[0];
  for (unsigned I = 1; I < Src.Elts.size(); ++I)
    Partial = combineReduction(O.Red, ResultTy, Partial, Src.Elts[I]);

  // One 32-slot window per reduction.  Address space 2 is the block's
  // shared memory, and the backend assigns this global a static offset.
  auto *SlotTy = ArrayType::get(ElemTy, ThreadsPerBlock);
  std::string Name = ("__gpumin_red_" + TF.getName() + "_" +
                      Twine(ReduceCounter++))
                         .str();
  auto *Window = new GlobalVariable(
      M, SlotTy, false, GlobalValue::InternalLinkage,
      Constant::getNullValue(SlotTy), Name, nullptr,
      GlobalValue::NotThreadLocal, 2);
  Window->setAlignment(Align(4));

  FunctionCallee TidFn = M.getOrInsertFunction("llvm.gpumin.tid",
                                               B.getInt32Ty());
  LlvmValue *Tid = B.CreateCall(TidFn);
  LlvmValue *Slot = B.CreateGEP(SlotTy, Window,
                                {B.getInt32(0), Tid});
  B.CreateStore(Partial, Slot);

  FunctionCallee BarrierFn =
      M.getOrInsertFunction("llvm.gpumin.barrier", B.getVoidTy());
  B.CreateCall(BarrierFn);

  BasicBlock *SumBB = BasicBlock::Create(Ctx, "red.sum", LF);
  BasicBlock *MergeBB = BasicBlock::Create(Ctx, "red.merge", LF);
  B.CreateCondBr(B.CreateICmpEQ(Tid, B.getInt32(0)), SumBB, MergeBB);

  B.SetInsertPoint(SumBB);
  LlvmValue *Base = B.CreateGEP(SlotTy, Window, {B.getInt32(0), B.getInt32(0)});
  LlvmValue *Acc = B.CreateLoad(ElemTy, Base);
  for (unsigned I = 1; I < ThreadsPerBlock; ++I) {
    LlvmValue *SlotI =
        B.CreateGEP(SlotTy, Window, {B.getInt32(0), B.getInt32(I)});
    Acc = combineReduction(O.Red, ResultTy, Acc,
                           B.CreateLoad(ElemTy, SlotI));
  }
  B.CreateStore(Acc, Base);
  B.CreateBr(MergeBB);

  B.SetInsertPoint(MergeBB);
  // The second barrier makes the lane-0 result visible to every thread, so
  // a reduction scalar behaves like any other SSA value.
  B.CreateCall(BarrierFn);
  LlvmValue *Result = B.CreateLoad(ElemTy, Base);
  Env[O.Result] = {{Result}, false};
  return Error::success();
}

Error FuncEmitter::emitReduce2D(const Op &O) {
  const Lowered &Src = Env.at(O.Operands[0]);
  Type SrcTy = O.Operands[0]->getType();
  Type ElemTy = SrcTy.getElementType();
  llvm::ArrayRef<int64_t> Shape = SrcTy.getShape();
  int64_t R = Shape[0], C = Shape[1];
  Expected<std::pair<int64_t, int64_t>> LayoutOrErr = threadLayout(SrcTy);
  if (!LayoutOrErr)
    return LayoutOrErr.takeError();
  int64_t Rows = LayoutOrErr->first, Cols = LayoutOrErr->second;

  // The whole tile goes to shared memory, so every output element can read
  // the values it needs without caring which thread loaded them.
  llvm::Type *LlvmElemTy = llvmType(ElemTy);
  auto *SlotTy = ArrayType::get(LlvmElemTy, static_cast<uint64_t>(R * C));
  std::string Name =
      ("__gpumin_red2d_" + TF.getName() + "_" + Twine(ReduceCounter++)).str();
  auto *Window =
      new GlobalVariable(M, SlotTy, false, GlobalValue::InternalLinkage,
                         Constant::getNullValue(SlotTy), Name, nullptr,
                         GlobalValue::NotThreadLocal, 2);
  Window->setAlignment(Align(4));

  for (unsigned I = 0; I < Src.Elts.size(); ++I) {
    LlvmValue *Row = coordValue(SrcTy, I, true);
    LlvmValue *Col = coordValue(SrcTy, I, false);
    LlvmValue *Index = B.CreateAdd(B.CreateMul(Row, B.getInt32(C)), Col);
    LlvmValue *Slot = B.CreateGEP(SlotTy, Window, {B.getInt32(0), Index});
    B.CreateStore(Src.Elts[I], Slot);
  }

  FunctionCallee BarrierFn =
      M.getOrInsertFunction("llvm.gpumin.barrier", B.getVoidTy());
  B.CreateCall(BarrierFn);

  auto LoadShared = [&](LlvmValue *Row, LlvmValue *Col) {
    LlvmValue *Index = B.CreateAdd(B.CreateMul(Row, B.getInt32(C)), Col);
    LlvmValue *Slot = B.CreateGEP(SlotTy, Window, {B.getInt32(0), Index});
    return B.CreateLoad(LlvmElemTy, Slot);
  };

  Lowered V;
  V.IsTensor = true;
  if (O.Axis == 1) {
    // [R, C] -> [R]: one partial per thread-local row.  Every lane in the
    // warp computes the same sums, which is exactly what the [R, 1] layout
    // wants after expand_dims.
    for (int64_t I = 0; I < Rows; ++I) {
      LlvmValue *Row =
          B.CreateAdd(B.CreateMul(rowGroup(), B.getInt32(Rows)), B.getInt32(I));
      LlvmValue *Acc = LoadShared(Row, B.getInt32(0));
      for (int64_t Col = 1; Col < C; ++Col)
        Acc = combineReduction(O.Red, ElemTy, Acc,
                               LoadShared(Row, B.getInt32(Col)));
      V.Elts.push_back(Acc);
    }
  } else {
    // [R, C] -> [C]: one partial per thread-local column, replicated across
    // the four warps.
    for (int64_t I = 0; I < Cols; ++I) {
      LlvmValue *Col =
          B.CreateAdd(B.CreateMul(laneId(), B.getInt32(Cols)), B.getInt32(I));
      LlvmValue *Acc = LoadShared(B.getInt32(0), Col);
      for (int64_t Row = 1; Row < R; ++Row)
        Acc = combineReduction(O.Red, ElemTy, Acc,
                               LoadShared(B.getInt32(Row), Col));
      V.Elts.push_back(Acc);
    }
  }

  // The second barrier keeps a following reduction from overwriting the
  // window while a slow warp is still reading it.
  B.CreateCall(BarrierFn);
  Env[O.Result] = std::move(V);
  return Error::success();
}

Error FuncEmitter::emitPrologue() {
  LF = llvm::Function::Create(llvm::FunctionType::get(B.getVoidTy(), false),
                              llvm::GlobalValue::ExternalLinkage, TF.getName(),
                              M);
  LF->addFnAttr("gpumin_kernel");

  BasicBlock *Entry = BasicBlock::Create(Ctx, "entry", LF);
  B.SetInsertPoint(Entry);

  FunctionCallee ArgsFn = M.getOrInsertFunction("llvm.gpumin.args",
                                                B.getInt32Ty());
  LlvmValue *ArgsAddr = B.CreateCall(ArgsFn);
  llvm::Type *PtrTy = PointerType::get(Ctx, 1);
  LlvmValue *ArgsPtr = B.CreateIntToPtr(ArgsAddr, PtrTy);

  unsigned Index = 0;
  for (gpumintriton::Value *Arg : TF.getArgs()) {
    LlvmValue *Slot = Index == 0
                      ? ArgsPtr
                      : B.CreateGEP(B.getInt32Ty(), ArgsPtr, B.getInt32(Index));
    LlvmValue *Raw = B.CreateLoad(B.getInt32Ty(), Slot);
    if (Arg->getType().isPtr())
      Env[Arg] = {{B.CreateIntToPtr(Raw, PtrTy)}, false};
    else if (Arg->getType().isF32())
      // sr.args stores raw 32-bit words.  A float parameter is the same
      // word reinterpreted, so bitcast rather than converting its value.
      Env[Arg] = {{B.CreateBitCast(Raw, B.getFloatTy())}, false};
    else
      Env[Arg] = {{Raw}, false};
    ++Index;
  }
  return Error::success();
}

Error FuncEmitter::emitOp(const Op &O) {
  switch (O.Kind) {
  case OpKind::Constant:
    if (O.Ty.isF32())
      Env[O.Result] = {
          {ConstantFP::get(Ctx, APFloat(static_cast<float>(O.FImm)))}, false};
    else
      Env[O.Result] = {{B.getInt32(static_cast<uint32_t>(O.Imm))}, false};
    return Error::success();

  case OpKind::GetProgramId: {
    StringRef Name =
        O.Axis == 0 ? "llvm.gpumin.blockidx" : "llvm.gpumin.blockidy";
    FunctionCallee BlockIdFn = M.getOrInsertFunction(Name, B.getInt32Ty());
    Env[O.Result] = {{B.CreateCall(BlockIdFn)}, false};
    return Error::success();
  }

  case OpKind::ScfIf: {
    const Lowered &Cond = Env.at(O.Operands[0]);
    if (Cond.IsTensor || Cond.Elts.size() != 1)
      return createStringError(inconvertibleErrorCode(),
                               "an if condition must be a scalar i1");
    llvm::BasicBlock *ThenBB = BasicBlock::Create(Ctx, "if.then", LF);
    llvm::BasicBlock *ElseBB =
        O.ElseOps.empty() ? nullptr : BasicBlock::Create(Ctx, "if.else", LF);
    llvm::BasicBlock *MergeBB = BasicBlock::Create(Ctx, "if.merge", LF);
    B.CreateCondBr(Cond.Elts[0], ThenBB, ElseBB ? ElseBB : MergeBB);

    // Branch-local values must not leak into the merge block; the frontend
    // already refused assignments to names that existed before the if.
    const std::map<const gpumintriton::Value *, Lowered> SavedEnv = Env;
    B.SetInsertPoint(ThenBB);
    if (Error E = emitOps(O.ThenOps))
      return E;
    if (!B.GetInsertBlock()->hasTerminator())
      B.CreateBr(MergeBB);

    if (ElseBB) {
      B.SetInsertPoint(ElseBB);
      if (Error E = emitOps(O.ElseOps))
        return E;
      if (!B.GetInsertBlock()->hasTerminator())
        B.CreateBr(MergeBB);
    }

    Env = SavedEnv;
    B.SetInsertPoint(MergeBB);
    return Error::success();
  }

  case OpKind::Barrier: {
    FunctionCallee BarrierFn =
        M.getOrInsertFunction("llvm.gpumin.barrier", B.getVoidTy());
    B.CreateCall(BarrierFn);
    return Error::success();
  }

  case OpKind::SharedAlloc: {
    Type Ty = O.Result->getType();
    llvm::ArrayRef<int64_t> Shape = Ty.getShape();
    llvm::Type *ElemTy = llvmType(Ty.getElementType());
    auto *SlotTy =
        ArrayType::get(ElemTy, static_cast<uint64_t>(Shape[0] * Shape[1]));
    std::string Name =
        ("__gpumin_sh_" + TF.getName() + "_" + Twine(SharedCounter++)).str();
    auto *Window =
        new GlobalVariable(M, SlotTy, false, GlobalValue::InternalLinkage,
                           Constant::getNullValue(SlotTy), Name, nullptr,
                           GlobalValue::NotThreadLocal, 2);
    Window->setAlignment(Align(4));
    Lowered V;
    V.Shared = Window;
    Env[O.Result] = std::move(V);
    return Error::success();
  }

  case OpKind::SharedStore:
    return emitSharedStore(O);

  case OpKind::SharedCol:
  case OpKind::SharedRow:
    Env[O.Result] = emitSharedSlice(O);
    return Error::success();

  case OpKind::Dot:
    return emitDot(O);

  case OpKind::Exp2: {
    const Lowered &Src = Env.at(O.Operands[0]);
    FunctionCallee Exp2Fn = M.getOrInsertFunction(
        "llvm.gpumin.exp2", B.getFloatTy(), B.getFloatTy());
    Lowered V;
    V.IsTensor = true;
    for (LlvmValue *Elt : Src.Elts)
      V.Elts.push_back(B.CreateCall(Exp2Fn, {Elt}));
    Env[O.Result] = std::move(V);
    return Error::success();
  }

  case OpKind::MakeRange: {
    FunctionCallee TidFn = M.getOrInsertFunction("llvm.gpumin.tid",
                                                 B.getInt32Ty());
    LlvmValue *Tid = B.CreateCall(TidFn);
    int64_t Range = O.End - O.Start;
    if (Range <= 0)
      return createStringError(inconvertibleErrorCode(),
                               "tl.arange range must be positive, got %lld",
                               static_cast<long long>(Range));
    if (Range < static_cast<int64_t>(ThreadsPerBlock)) {
      // Small ranges only exist to feed [:, None] / [None, :]; expanding them
      // later computes each thread's values directly, so there is no 1D
      // distribution to materialise here.  emit() rejects any other consumer.
      Lowered V;
      V.IsTensor = true;
      Env[O.Result] = std::move(V);
      return Error::success();
    }
    if (Range % ThreadsPerBlock != 0)
      return createStringError(inconvertibleErrorCode(),
                               "tensor size must be a positive multiple of %u, "
                               "got %lld",
                               ThreadsPerBlock, static_cast<long long>(Range));
    unsigned Count = static_cast<unsigned>(Range / ThreadsPerBlock);
    Lowered V;
    V.IsTensor = true;
    for (unsigned R = 0; R < Count; ++R) {
      LlvmValue *Base = B.getInt32(static_cast<uint32_t>(O.Start + R * ThreadsPerBlock));
      V.Elts.push_back(B.CreateAdd(Base, Tid));
    }
    Env[O.Result] = std::move(V);
    return Error::success();
  }

  case OpKind::ExpandDims: {
    auto It = Defs.find(O.Operands[0]);
    if (It != Defs.end() && It->second->Kind == OpKind::Reduce) {
      // A 2D reduction produces a rank-1 tt.reduce result that has no 1D
      // layout of its own.  keep_dims=True expands it right back, and the
      // per-thread element order is already the expanded order, so this is
      // an identity on the register vector.
      const Op &Red = *It->second;
      llvm::ArrayRef<int64_t> SrcShape = Red.Operands[0]->getType().getShape();
      llvm::ArrayRef<int64_t> DstShape = O.Ty.getShape();
      if (SrcShape.size() != 2 || Red.Axis != O.Axis || DstShape.size() != 2)
        return createStringError(inconvertibleErrorCode(),
                                 "a deferred 2D reduction must expand on its "
                                 "reduced axis");
      Env[O.Result] = Env.at(O.Operands[0]);
      return Error::success();
    }
    if (It == Defs.end() || It->second->Kind != OpKind::MakeRange)
      return createStringError(inconvertibleErrorCode(),
                               "expand_dims only reshapes tl.arange or a 2D "
                               "reduction in this chapter");
    const Op &Range = *It->second;
    int64_t Start = Range.Start, RangeLen = Range.End - Range.Start;
    llvm::ArrayRef<int64_t> Shape = O.Ty.getShape();
    int64_t WantLen = O.Axis == 0 ? Shape[1] : Shape[0];
    if (RangeLen != WantLen)
      return createStringError(inconvertibleErrorCode(),
                               "expand_dims shape does not match the "
                               "tl.arange length");
    Expected<unsigned> CountOrErr = elementsPerThread(O.Ty);
    if (!CountOrErr)
      return CountOrErr.takeError();
    Lowered V;
    V.IsTensor = true;
    for (unsigned I = 0; I < *CountOrErr; ++I) {
      LlvmValue *Index = coordValue(O.Ty, I, O.Axis == 1);
      V.Elts.push_back(
          B.CreateAdd(B.getInt32(static_cast<uint32_t>(Start)), Index));
    }
    Env[O.Result] = std::move(V);
    return Error::success();
  }

  case OpKind::Broadcast: {
    Type SrcTy = O.Operands[0]->getType();
    Type DstTy = O.Result->getType();
    llvm::ArrayRef<int64_t> S = SrcTy.getShape();
    llvm::ArrayRef<int64_t> D = DstTy.getShape();
    if (S.size() != 2 || D.size() != 2)
      return createStringError(inconvertibleErrorCode(),
                               "broadcast needs 2D tensors");
    const Lowered &Src = Env.at(O.Operands[0]);
    Lowered V;
    V.IsTensor = true;
    if (S == D) {
      V.Elts = Src.Elts;
    } else if (S[0] == D[0] && S[1] == 1) {
      Expected<std::pair<int64_t, int64_t>> LayoutOrErr = threadLayout(DstTy);
      if (!LayoutOrErr)
        return LayoutOrErr.takeError();
      int64_t Cols = LayoutOrErr->second;
      for (LlvmValue *Elt : Src.Elts)
        for (int64_t C = 0; C < Cols; ++C)
          V.Elts.push_back(Elt);
    } else if (S[0] == 1 && S[1] == D[1]) {
      Expected<std::pair<int64_t, int64_t>> LayoutOrErr = threadLayout(DstTy);
      if (!LayoutOrErr)
        return LayoutOrErr.takeError();
      int64_t Rows = LayoutOrErr->first;
      for (int64_t R = 0; R < Rows; ++R)
        for (LlvmValue *Elt : Src.Elts)
          V.Elts.push_back(Elt);
    } else if (S[0] == 1 && S[1] == 1) {
      Expected<unsigned> CountOrErr = elementsPerThread(DstTy);
      if (!CountOrErr)
        return CountOrErr.takeError();
      V.Elts.assign(*CountOrErr, Src.Elts[0]);
    } else {
      return createStringError(inconvertibleErrorCode(),
                               "only broadcasting from a size-1 dimension is "
                               "supported");
    }
    Env[O.Result] = std::move(V);
    return Error::success();
  }

  case OpKind::Trans: {
    Type SrcTy = O.Operands[0]->getType();
    Type DstTy = O.Result->getType();
    llvm::ArrayRef<int64_t> S = SrcTy.getShape();
    if (S.size() != 2 || S[0] <= 1 || S[1] <= 1)
      return createStringError(inconvertibleErrorCode(),
                               "transpose requires a 2D tensor with both "
                               "dimensions greater than 1");
    Expected<std::pair<int64_t, int64_t>> SrcLayout = threadLayout(SrcTy);
    if (!SrcLayout)
      return SrcLayout.takeError();
    Expected<std::pair<int64_t, int64_t>> DstLayout = threadLayout(DstTy);
    if (!DstLayout)
      return DstLayout.takeError();
    const Lowered &Src = Env.at(O.Operands[0]);
    int64_t R = S[0], C = S[1];
    llvm::Type *ElemTy = llvmType(SrcTy.getElementType());
    auto *SlotTy = ArrayType::get(ElemTy, static_cast<uint64_t>(R * C));
    std::string Name =
        ("__gpumin_trans_" + TF.getName() + "_" + Twine(TransCounter++)).str();
    auto *Window =
        new GlobalVariable(M, SlotTy, false, GlobalValue::InternalLinkage,
                           Constant::getNullValue(SlotTy), Name, nullptr,
                           GlobalValue::NotThreadLocal, 2);
    Window->setAlignment(Align(4));

    auto *Flat = B.getInt32(0);
    for (unsigned I = 0; I < Src.Elts.size(); ++I) {
      LlvmValue *Row = coordValue(SrcTy, I, true);
      LlvmValue *Col = coordValue(SrcTy, I, false);
      LlvmValue *Index = B.CreateAdd(B.CreateMul(Row, B.getInt32(C)), Col);
      LlvmValue *Slot = B.CreateGEP(SlotTy, Window, {Flat, Index});
      B.CreateStore(Src.Elts[I], Slot);
    }
    FunctionCallee BarrierFn =
        M.getOrInsertFunction("llvm.gpumin.barrier", B.getVoidTy());
    B.CreateCall(BarrierFn);

    Expected<unsigned> CountOrErr = elementsPerThread(DstTy);
    if (!CountOrErr)
      return CountOrErr.takeError();
    Lowered V;
    V.IsTensor = true;
    for (unsigned I = 0; I < *CountOrErr; ++I) {
      LlvmValue *Row = coordValue(DstTy, I, true);  // C-direction
      LlvmValue *Col = coordValue(DstTy, I, false); // R-direction
      LlvmValue *Index = B.CreateAdd(B.CreateMul(Col, B.getInt32(C)), Row);
      LlvmValue *Slot = B.CreateGEP(SlotTy, Window, {Flat, Index});
      V.Elts.push_back(B.CreateLoad(ElemTy, Slot));
    }
    B.CreateCall(BarrierFn);
    Env[O.Result] = std::move(V);
    return Error::success();
  }

  case OpKind::Splat: {
    LlvmValue *Scalar = elementAt(Env.at(O.Operands[0]), 0);
    Expected<unsigned> CountOrErr = elementsPerThread(O.Ty);
    if (!CountOrErr)
      return CountOrErr.takeError();
    unsigned Count = *CountOrErr;
    Lowered V;
    V.IsTensor = true;
    V.Elts.assign(Count, Scalar);
    Env[O.Result] = std::move(V);
    return Error::success();
  }

  case OpKind::AddPtr: {
    const Lowered &Ptr = Env.at(O.Operands[0]);
    const Lowered &Off = Env.at(O.Operands[1]);
    unsigned Count = std::max(Ptr.Elts.size(), Off.Elts.size());
    Lowered V;
    V.IsTensor = Ptr.IsTensor || Off.IsTensor;
    for (unsigned R = 0; R < Count; ++R)
      V.Elts.push_back(B.CreateGEP(B.getInt32Ty(), elementAt(Ptr, R),
                                   elementAt(Off, R)));
    Env[O.Result] = std::move(V);
    return Error::success();
  }

  case OpKind::Load: {
    const Lowered &Ptr = Env.at(O.Operands[0]);
    const Lowered *Mask =
        O.Operands.size() > 1 ? &Env.at(O.Operands[1]) : nullptr;
    const Lowered *Other =
        O.Operands.size() > 2 ? &Env.at(O.Operands[2]) : nullptr;
    Type ResultTy = O.Result->getType();
    Type ElemTy = ResultTy.isTensor() ? ResultTy.getElementType() : ResultTy;
    llvm::Type *ElemLlvmTy = llvmType(ElemTy);
    Lowered V;
    V.IsTensor = Ptr.IsTensor;
    for (unsigned R = 0; R < Ptr.Elts.size(); ++R) {
      LlvmValue *MaskElt = Mask ? elementAt(*Mask, R) : nullptr;
      LlvmValue *OtherElt = Other ? elementAt(*Other, R) : nullptr;
      V.Elts.push_back(maskedLoad(Ptr.Elts[R], MaskElt, OtherElt, ElemLlvmTy));
    }
    Env[O.Result] = std::move(V);
    return Error::success();
  }

  case OpKind::Store: {
    const Lowered &Ptr = Env.at(O.Operands[0]);
    const Lowered &Stored = Env.at(O.Operands[1]);
    const Lowered *Mask =
        O.Operands.size() > 2 ? &Env.at(O.Operands[2]) : nullptr;
    for (unsigned R = 0; R < Ptr.Elts.size(); ++R) {
      LlvmValue *MaskElt = Mask ? elementAt(*Mask, R) : nullptr;
      maskedStore(Ptr.Elts[R], elementAt(Stored, R), MaskElt);
    }
    return Error::success();
  }

  case OpKind::CmpI: {
    const Lowered &L = Env.at(O.Operands[0]);
    const Lowered &RHS = Env.at(O.Operands[1]);
    CmpInst::Predicate Pred;
    switch (O.Pred) {
    case CmpPred::Eq:
      Pred = CmpInst::ICMP_EQ;
      break;
    case CmpPred::Ne:
      Pred = CmpInst::ICMP_NE;
      break;
    case CmpPred::Slt:
      Pred = CmpInst::ICMP_SLT;
      break;
    case CmpPred::Sle:
      Pred = CmpInst::ICMP_SLE;
      break;
    case CmpPred::Sgt:
      Pred = CmpInst::ICMP_SGT;
      break;
    case CmpPred::Sge:
      Pred = CmpInst::ICMP_SGE;
      break;
    case CmpPred::Ult:
      Pred = CmpInst::ICMP_ULT;
      break;
    case CmpPred::Ule:
      Pred = CmpInst::ICMP_ULE;
      break;
    case CmpPred::Ugt:
      Pred = CmpInst::ICMP_UGT;
      break;
    case CmpPred::Uge:
      Pred = CmpInst::ICMP_UGE;
      break;
    }
    Lowered V;
    V.IsTensor = L.IsTensor;
    for (unsigned I = 0; I < L.Elts.size(); ++I)
      V.Elts.push_back(B.CreateICmp(Pred, elementAt(L, I),
                                    elementAt(RHS, I)));
    Env[O.Result] = std::move(V);
    return Error::success();
  }

  case OpKind::Reduce:
    return emitReduce(O);

  case OpKind::Select: {
    const Lowered &Cond = Env.at(O.Operands[0]);
    const Lowered &TrueV = Env.at(O.Operands[1]);
    const Lowered &FalseV = Env.at(O.Operands[2]);
    Lowered V;
    V.IsTensor = TrueV.IsTensor;
    for (unsigned I = 0; I < TrueV.Elts.size(); ++I)
      V.Elts.push_back(B.CreateSelect(elementAt(Cond, I),
                                      elementAt(TrueV, I),
                                      elementAt(FalseV, I)));
    Env[O.Result] = std::move(V);
    return Error::success();
  }

  case OpKind::CmpF: {
    const Lowered &L = Env.at(O.Operands[0]);
    const Lowered &RHS = Env.at(O.Operands[1]);
    FCmpInst::Predicate Pred;
    switch (O.Pred) {
    case CmpPred::Eq:
      Pred = FCmpInst::FCMP_OEQ;
      break;
    case CmpPred::Ne:
      Pred = FCmpInst::FCMP_ONE;
      break;
    case CmpPred::Slt:
      Pred = FCmpInst::FCMP_OLT;
      break;
    case CmpPred::Sle:
      Pred = FCmpInst::FCMP_OLE;
      break;
    case CmpPred::Sgt:
      Pred = FCmpInst::FCMP_OGT;
      break;
    case CmpPred::Sge:
      Pred = FCmpInst::FCMP_OGE;
      break;
    case CmpPred::Ult:
    case CmpPred::Ule:
    case CmpPred::Ugt:
    case CmpPred::Uge:
      llvm_unreachable("unsigned predicates do not exist for f32");
    }
    Lowered V;
    V.IsTensor = L.IsTensor;
    for (unsigned I = 0; I < L.Elts.size(); ++I)
      V.Elts.push_back(B.CreateFCmp(Pred, elementAt(L, I),
                                    elementAt(RHS, I)));
    Env[O.Result] = std::move(V);
    return Error::success();
  }

  case OpKind::Return:
    B.CreateRetVoid();
    return Error::success();

  default: {
    const Lowered &L = Env.at(O.Operands[0]);
    const Lowered &RHS = Env.at(O.Operands[1]);
    Lowered V;
    V.IsTensor = L.IsTensor;
    for (unsigned I = 0; I < L.Elts.size(); ++I) {
      LlvmValue *A = elementAt(L, I);
      LlvmValue *Bv = elementAt(RHS, I);
      switch (O.Kind) {
      case OpKind::AddI:
        V.Elts.push_back(B.CreateAdd(A, Bv));
        break;
      case OpKind::SubI:
        V.Elts.push_back(B.CreateSub(A, Bv));
        break;
      case OpKind::MulI:
        V.Elts.push_back(B.CreateMul(A, Bv));
        break;
      case OpKind::DivI:
        V.Elts.push_back(B.CreateSDiv(A, Bv));
        break;
      case OpKind::RemI:
        V.Elts.push_back(B.CreateSRem(A, Bv));
        break;
      case OpKind::AddF:
        V.Elts.push_back(B.CreateFAdd(A, Bv));
        break;
      case OpKind::SubF:
        V.Elts.push_back(B.CreateFSub(A, Bv));
        break;
      case OpKind::MulF:
        V.Elts.push_back(B.CreateFMul(A, Bv));
        break;
      case OpKind::DivF:
        V.Elts.push_back(B.CreateFDiv(A, Bv));
        break;
      case OpKind::AndI:
        V.Elts.push_back(B.CreateAnd(A, Bv));
        break;
      case OpKind::OrI:
        V.Elts.push_back(B.CreateOr(A, Bv));
        break;
      case OpKind::XOrI:
        V.Elts.push_back(B.CreateXor(A, Bv));
        break;
      case OpKind::ShlI:
        V.Elts.push_back(B.CreateShl(A, Bv));
        break;
      case OpKind::ShrI:
        V.Elts.push_back(B.CreateLShr(A, Bv));
        break;
      case OpKind::AShrI:
        V.Elts.push_back(B.CreateAShr(A, Bv));
        break;
      default:
        llvm_unreachable("LLVMEmit: unknown binary op");
      }
    }
    Env[O.Result] = std::move(V);
    return Error::success();
  }
  }
}

/// Depth-first walk over top-level ops and scf.if regions.
static void collectOps(llvm::ArrayRef<Op> Ops, std::vector<const Op *> &Out) {
  for (const Op &O : Ops) {
    Out.push_back(&O);
    collectOps(O.ThenOps, Out);
    collectOps(O.ElseOps, Out);
  }
}

Error FuncEmitter::emitOps(llvm::ArrayRef<Op> Ops) {
  for (const Op &O : Ops)
    if (Error E = emitOp(O))
      return E;
  return Error::success();
}

Error FuncEmitter::emit() {
  if (Error E = emitPrologue())
    return E;
  std::vector<const Op *> AllOps;
  collectOps(TF.getOps(), AllOps);
  for (const Op *O : AllOps)
    if (O->Result)
      Defs[O->Result] = O;
  for (const Op *O : AllOps) {
    if (O->Kind != OpKind::MakeRange ||
        (O->End - O->Start) >= static_cast<int64_t>(ThreadsPerBlock))
      continue;
    for (const Op *User : AllOps)
      for (const gpumintriton::Value *Operand : User->Operands)
        if (Operand == O->Result && User->Kind != OpKind::ExpandDims)
          return createStringError(
              inconvertibleErrorCode(),
              "a tl.arange shorter than %u must feed [:, None] or [None, :]",
              ThreadsPerBlock);
  }
  for (const Op *O : AllOps) {
    if (O->Kind != OpKind::Reduce ||
        O->Operands[0]->getType().getShape().size() != 2)
      continue;
    for (const Op *User : AllOps)
      for (const gpumintriton::Value *Operand : User->Operands)
        if (Operand == O->Result && User->Kind != OpKind::ExpandDims)
          return createStringError(
              inconvertibleErrorCode(),
              "a 2D reduction result must feed expand_dims "
              "(keep_dims=True)");
  }
  if (Error E = emitOps(TF.getOps()))
    return E;
  // A TTIR function without an explicit return still needs a terminator.
  if (!B.GetInsertBlock()->hasTerminator())
    B.CreateRetVoid();
  return Error::success();
}

} // namespace

llvm::Expected<std::unique_ptr<llvm::Module>>
emitLLVM(const Module &TTIRModule, LLVMContext &Ctx) {
  auto M = std::make_unique<llvm::Module>("gpumin-triton", Ctx);
  M->setTargetTriple(Triple("gpumin-unknown-elf"));
  for (const Function &F : TTIRModule.getFunctions()) {
    FuncEmitter E(*M, F);
    if (Error Err = E.emit())
      return std::move(Err);
  }
  return std::move(M);
}

} // namespace gpumintriton
