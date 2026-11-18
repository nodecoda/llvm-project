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

struct Lowered {
  std::vector<LlvmValue *> Elts;
  bool IsTensor = false;
};

class FuncEmitter {
public:
  FuncEmitter(llvm::Module &M, const Function &TF)
      : M(M), TF(TF), Ctx(M.getContext()), B(Ctx) {}
  Error emit();

private:
  Expected<unsigned> elementsPerThread(Type Ty) const;
  llvm::Type *llvmType(Type Ty);
  LlvmValue *elementAt(const Lowered &V, unsigned R) const;
  LlvmValue *maskedLoad(LlvmValue *Ptr, LlvmValue *Mask, llvm::Type *ElemTy);
  LlvmValue *combineReduction(ReduceKind Kind, Type ElemTy, LlvmValue *A,
                              LlvmValue *Rhs);
  Error emitReduce(const Op &O);
  void maskedStore(LlvmValue *Ptr, LlvmValue *Stored, LlvmValue *Mask);
  Error emitOp(const Op &O);
  Error emitPrologue();

  llvm::Module &M;
  const Function &TF;
  LLVMContext &Ctx;
  IRBuilder<> B;
  llvm::Function *LF = nullptr;
  std::map<const gpumintriton::Value *, Lowered> Env;
  unsigned ReduceCounter = 0;
};

Expected<unsigned> FuncEmitter::elementsPerThread(Type Ty) const {
  if (!Ty.isTensor())
    return 1;
  int64_t N = Ty.getShape()[0];
  if (Ty.getShape().size() != 1 || N <= 0 || N % ThreadsPerBlock != 0)
    return createStringError(inconvertibleErrorCode(),
                             "tensor size must be a positive multiple of %u, "
                             "got %lld",
                             ThreadsPerBlock, static_cast<long long>(N));
  return static_cast<unsigned>(N / ThreadsPerBlock);
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
                                   llvm::Type *ElemTy) {
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
  Phi->addIncoming(Constant::getNullValue(ElemTy), Before);
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

Error FuncEmitter::emitReduce(const Op &O) {
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
    FunctionCallee BlockIdFn = M.getOrInsertFunction(
        "llvm.gpumin.blockid", B.getInt32Ty());
    Env[O.Result] = {{B.CreateCall(BlockIdFn)}, false};
    return Error::success();
  }

  case OpKind::MakeRange: {
    FunctionCallee TidFn = M.getOrInsertFunction("llvm.gpumin.tid",
                                                 B.getInt32Ty());
    LlvmValue *Tid = B.CreateCall(TidFn);
    int64_t Range = O.End - O.Start;
    if (Range <= 0 || Range % ThreadsPerBlock != 0)
      return createStringError(inconvertibleErrorCode(),
                               "tl.arange range must be a positive multiple "
                               "of %u, got %lld",
                               ThreadsPerBlock,
                               static_cast<long long>(Range));
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
    Type ResultTy = O.Result->getType();
    Type ElemTy = ResultTy.isTensor() ? ResultTy.getElementType() : ResultTy;
    llvm::Type *ElemLlvmTy = llvmType(ElemTy);
    Lowered V;
    V.IsTensor = Ptr.IsTensor;
    for (unsigned R = 0; R < Ptr.Elts.size(); ++R) {
      LlvmValue *MaskElt = Mask ? elementAt(*Mask, R) : nullptr;
      V.Elts.push_back(maskedLoad(Ptr.Elts[R], MaskElt, ElemLlvmTy));
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

Error FuncEmitter::emit() {
  if (Error E = emitPrologue())
    return E;
  for (const Op &O : TF.getOps()) {
    if (Error E = emitOp(O))
      return E;
  }
  // A TTIR function without an explicit return still needs a terminator.
  if (!B.GetInsertBlock()->getTerminator())
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
