//===-- TTIR.cpp - TTIR-min for gpumin-triton -----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "TTIR.h"

#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/Format.h"
#include <cassert>

using namespace llvm;

namespace gpumintriton {

Type Type::i1() {
  Type T;
  T.K = Kind::I1;
  return T;
}

Type Type::i32() {
  Type T;
  T.K = Kind::I32;
  return T;
}

Type Type::f32() {
  Type T;
  T.K = Kind::F32;
  return T;
}

Type Type::ptr(Type Pointee, unsigned AddressSpace) {
  Type T;
  T.K = Kind::Ptr;
  T.AddressSpace = AddressSpace;
  T.Elem = std::make_shared<Type>(Pointee);
  return T;
}

Type Type::tensor(ArrayRef<int64_t> Shape, Type Element) {
  Type T;
  T.K = Kind::Tensor;
  T.Elem = std::make_shared<Type>(Element);
  T.Shape.append(Shape.begin(), Shape.end());
  return T;
}

Type Type::getPointee() const {
  assert(K == Kind::Ptr && Elem && "not a pointer type");
  return *Elem;
}

Type Type::getElementType() const {
  assert(K == Kind::Tensor && Elem && "not a tensor type");
  return *Elem;
}

void Type::print(raw_ostream &OS) const {
  switch (K) {
  case Kind::I1:
    OS << "i1";
    return;
  case Kind::I32:
    OS << "i32";
    return;
  case Kind::F32:
    OS << "f32";
    return;
  case Kind::Ptr:
    OS << "!tt.ptr<";
    Elem->print(OS);
    OS << ">";
    return;
  case Kind::Tensor:
    OS << "tensor<";
    for (size_t I = 0; I < Shape.size(); ++I) {
      if (I)
        OS << "x";
      OS << Shape[I];
    }
    OS << "x";
    Elem->print(OS);
    OS << ">";
    return;
  }
}

bool Type::operator==(const Type &Other) const {
  if (K != Other.K || AddressSpace != Other.AddressSpace ||
      Shape != Other.Shape)
    return false;
  if (static_cast<bool>(Elem) != static_cast<bool>(Other.Elem))
    return false;
  return !Elem || *Elem == *Other.Elem;
}

void Value::printName(raw_ostream &OS) const {
  OS << "%";
  if (!Name.empty())
    OS << Name;
  else
    OS << ID;
}

Value *Function::createValue(Type Ty) {
  Values.emplace_back(new Value(NextID++, Ty));
  return Values.back().get();
}

Value *Function::addArg(StringRef ArgName, Type Ty) {
  // Arguments are printed by name and do not consume an SSA number; Triton's
  // TTIR numbering starts at the first operation result.
  Values.emplace_back(new Value(0, Ty));
  Value *V = Values.back().get();
  V->setName(ArgName);
  Args.push_back(V);
  return V;
}

Value *Function::createConstant(int64_t Imm, Type Ty) {
  Value *V = createValue(Ty);
  Op O;
  O.Kind = OpKind::Constant;
  O.Result = V;
  O.Imm = Imm;
  O.Ty = Ty;
  Ops.push_back(std::move(O));
  return V;
}

Value *Function::createConstant(double FImm, Type Ty) {
  assert(Ty.isF32() && "float constant must be f32");
  Value *V = createValue(Ty);
  Op O;
  O.Kind = OpKind::Constant;
  O.Result = V;
  O.FImm = FImm;
  O.Ty = Ty;
  Ops.push_back(std::move(O));
  return V;
}

Value *Function::createBinary(OpKind Kind, Value *LHS, Value *RHS) {
  assert(LHS->getType() == RHS->getType() && "binary operand type mismatch");
  Value *V = createValue(LHS->getType());
  Op O;
  O.Kind = Kind;
  O.Result = V;
  O.Operands = {LHS, RHS};
  O.Ty = LHS->getType();
  Ops.push_back(std::move(O));
  return V;
}

Value *Function::createCmp(CmpPred Pred, Value *LHS, Value *RHS) {
  assert(LHS->getType() == RHS->getType() && "cmp operand type mismatch");
  Type ResultTy = Type::i1();
  if (LHS->getType().isTensor())
    ResultTy = Type::tensor(LHS->getType().getShape(), Type::i1());
  Type OperandTy = LHS->getType();
  Type ElemTy = OperandTy.isTensor() ? OperandTy.getElementType() : OperandTy;
  Value *V = createValue(ResultTy);
  Op O;
  O.Kind = ElemTy.isF32() ? OpKind::CmpF : OpKind::CmpI;
  O.Result = V;
  O.Operands = {LHS, RHS};
  O.Pred = Pred;
  O.Ty = LHS->getType();
  Ops.push_back(std::move(O));
  return V;
}

Value *Function::createGetProgramId(unsigned Axis) {
  Value *V = createValue(Type::i32());
  Op O;
  O.Kind = OpKind::GetProgramId;
  O.Result = V;
  O.Axis = Axis;
  O.Ty = Type::i32();
  Ops.push_back(std::move(O));
  return V;
}

Value *Function::createMakeRange(int64_t Start, int64_t End) {
  assert(End >= Start && "make_range with end < start");
  Type ResultTy = Type::tensor({End - Start}, Type::i32());
  Value *V = createValue(ResultTy);
  Op O;
  O.Kind = OpKind::MakeRange;
  O.Result = V;
  O.Start = Start;
  O.End = End;
  O.Ty = ResultTy;
  Ops.push_back(std::move(O));
  return V;
}

Value *Function::createExpandDims(Value *Src, unsigned Axis) {
  assert(Src->getType().isTensor() && Src->getType().getShape().size() == 1 &&
         "expand_dims needs a 1D tensor");
  assert(Axis <= 1 && "expand_dims only supports axis 0 or 1");
  ArrayRef<int64_t> SrcShape = Src->getType().getShape();
  Type DstTy =
      Axis == 0
          ? Type::tensor({1, SrcShape[0]}, Src->getType().getElementType())
          : Type::tensor({SrcShape[0], 1}, Src->getType().getElementType());
  Value *V = createValue(DstTy);
  Op O;
  O.Kind = OpKind::ExpandDims;
  O.Result = V;
  O.Operands = {Src};
  O.Axis = Axis;
  O.Ty = DstTy;
  Ops.push_back(std::move(O));
  return V;
}

Value *Function::createBroadcast(Value *Src, Type DstTy) {
  assert(Src->getType().isTensor() && DstTy.isTensor() &&
         "broadcast needs tensors");
  Value *V = createValue(DstTy);
  Op O;
  O.Kind = OpKind::Broadcast;
  O.Result = V;
  O.Operands = {Src};
  O.Ty = DstTy;
  Ops.push_back(std::move(O));
  return V;
}

Value *Function::createTrans(Value *Src) {
  assert(Src->getType().isTensor() && Src->getType().getShape().size() == 2 &&
         "trans needs a 2D tensor");
  ArrayRef<int64_t> S = Src->getType().getShape();
  Type DstTy = Type::tensor({S[1], S[0]}, Src->getType().getElementType());
  Value *V = createValue(DstTy);
  Op O;
  O.Kind = OpKind::Trans;
  O.Result = V;
  O.Operands = {Src};
  O.Axis = 1; // order = [1, 0]
  O.Ty = DstTy;
  Ops.push_back(std::move(O));
  return V;
}

Value *Function::createSplat(Value *Scalar, Type TensorTy) {
  assert(TensorTy.isTensor() && "splat result must be a tensor");
  assert(TensorTy.getElementType() == Scalar->getType() &&
         "splat element type mismatch");
  Value *V = createValue(TensorTy);
  Op O;
  O.Kind = OpKind::Splat;
  O.Result = V;
  O.Operands = {Scalar};
  O.Ty = TensorTy;
  Ops.push_back(std::move(O));
  return V;
}

Value *Function::createAddPtr(Value *Ptr, Value *Offset) {
  Type PtrTy = Ptr->getType();
  assert((PtrTy.isPtr() || PtrTy.isTensor()) && "addptr needs a pointer");
  if (PtrTy.isTensor())
    assert(PtrTy.getElementType().isPtr() && "addptr needs pointer elements");
  Value *V = createValue(PtrTy);
  Op O;
  O.Kind = OpKind::AddPtr;
  O.Result = V;
  O.Operands = {Ptr, Offset};
  O.Ty = PtrTy;
  Ops.push_back(std::move(O));
  return V;
}

Value *Function::createLoad(Value *Ptr, Value *Mask) {
  Type PtrTy = Ptr->getType();
  assert((PtrTy.isPtr() || PtrTy.isTensor()) && "load needs a pointer");
  Type ElemTy;
  if (PtrTy.isTensor()) {
    assert(PtrTy.getElementType().isPtr() && "load needs pointer elements");
    ElemTy = Type::tensor(PtrTy.getShape(), PtrTy.getElementType().getPointee());
  } else {
    ElemTy = PtrTy.getPointee();
  }
  Value *V = createValue(ElemTy);
  Op O;
  O.Kind = OpKind::Load;
  O.Result = V;
  O.Operands = {Ptr};
  if (Mask)
    O.Operands.push_back(Mask);
  O.Ty = PtrTy;
  Ops.push_back(std::move(O));
  return V;
}

Value *Function::createReduce(ReduceKind Kind, Value *Src, unsigned Axis) {
  assert(Src->getType().isTensor() && "reduce needs a tensor");
  llvm::ArrayRef<int64_t> Shape = Src->getType().getShape();
  assert((Shape.size() == 1 || Shape.size() == 2) &&
         "only 1-D and 2-D reductions");
  Type ResultTy;
  if (Shape.size() == 1) {
    ResultTy = Src->getType().getElementType();
  } else {
    assert(Axis <= 1 && "a 2-D reduction uses axis 0 or 1");
    // Triton's tt.reduce always drops the reduced dimension; keep_dims=True
    // is expressed by a following tt.expand_dims (chapter 31).
    ResultTy = Type::tensor({Shape[Axis == 0 ? 1 : 0]},
                            Src->getType().getElementType());
  }
  Value *V = createValue(ResultTy);
  Op O;
  O.Kind = OpKind::Reduce;
  O.Result = V;
  O.Operands = {Src};
  O.Axis = Axis;
  O.Red = Kind;
  O.Ty = Src->getType();
  Ops.push_back(std::move(O));
  return V;
}

Value *Function::createExp2(Value *Src) {
  assert(Src->getType().isTensor() && "exp2 needs a tensor");
  assert(Src->getType().getElementType().isF32() && "exp2 needs f32");
  Value *V = createValue(Src->getType());
  Op O;
  O.Kind = OpKind::Exp2;
  O.Result = V;
  O.Operands = {Src};
  O.Ty = Src->getType();
  Ops.push_back(std::move(O));
  return V;
}

Value *Function::createSelect(Value *Cond, Value *TrueV, Value *FalseV) {
  assert(TrueV->getType() == FalseV->getType() && "select type mismatch");
  Value *V = createValue(TrueV->getType());
  Op O;
  O.Kind = OpKind::Select;
  O.Result = V;
  O.Operands = {Cond, TrueV, FalseV};
  O.Ty = TrueV->getType();
  Ops.push_back(std::move(O));
  return V;
}

void Function::createStore(Value *Ptr, Value *Stored, Value *Mask) {
  Op O;
  O.Kind = OpKind::Store;
  O.Operands = {Ptr, Stored};
  if (Mask)
    O.Operands.push_back(Mask);
  O.Ty = Ptr->getType();
  Ops.push_back(std::move(O));
}

std::vector<Op> Function::takeOpsFrom(size_t Mark) {
  std::vector<Op> Out;
  Out.reserve(Ops.size() - Mark);
  for (size_t I = Mark; I < Ops.size(); ++I)
    Out.push_back(std::move(Ops[I]));
  Ops.resize(Mark);
  return Out;
}

void Function::createScfIf(Value *Cond, std::vector<Op> ThenOps,
                           std::vector<Op> ElseOps) {
  Op O;
  O.Kind = OpKind::ScfIf;
  O.Operands = {Cond};
  O.ThenOps = std::move(ThenOps);
  O.ElseOps = std::move(ElseOps);
  Ops.push_back(std::move(O));
}

Value *Function::createSharedAlloc(Type TensorTy) {
  Value *V = createValue(TensorTy);
  V->markShared();
  Op O;
  O.Kind = OpKind::SharedAlloc;
  O.Result = V;
  O.Ty = TensorTy;
  Ops.push_back(std::move(O));
  return V;
}

void Function::createSharedStore(Value *Handle, Value *Src) {
  Op O;
  O.Kind = OpKind::SharedStore;
  O.Operands = {Handle, Src};
  Ops.push_back(std::move(O));
}

static Op makeSharedSlice(OpKind Kind, Value *Handle, int64_t Index) {
  Op O;
  O.Kind = Kind;
  O.Operands = {Handle};
  O.Imm = Index;
  Type HandleTy = Handle->getType();
  llvm::ArrayRef<int64_t> Shape = HandleTy.getShape();
  Type Elem = HandleTy.getElementType();
  O.Ty = Kind == OpKind::SharedCol ? Type::tensor({Shape[0], 1}, Elem)
                                   : Type::tensor({1, Shape[1]}, Elem);
  return O;
}

Value *Function::createSharedCol(Value *Handle, int64_t Index) {
  Op O = makeSharedSlice(OpKind::SharedCol, Handle, Index);
  O.Result = createValue(O.Ty);
  Value *V = O.Result;
  Ops.push_back(std::move(O));
  return V;
}

Value *Function::createSharedRow(Value *Handle, int64_t Index) {
  Op O = makeSharedSlice(OpKind::SharedRow, Handle, Index);
  O.Result = createValue(O.Ty);
  Value *V = O.Result;
  Ops.push_back(std::move(O));
  return V;
}

Value *Function::createDot(Value *A, Value *B, Value *Acc) {
  Type ATy = A->getType();
  Type BTy = B->getType();
  llvm::ArrayRef<int64_t> AS = ATy.getShape();
  llvm::ArrayRef<int64_t> BS = BTy.getShape();
  Type ResTy = Type::tensor({AS[0], BS[1]}, ATy.getElementType());
  Value *V = createValue(ResTy);
  Op O;
  O.Kind = OpKind::Dot;
  O.Result = V;
  O.Operands = {A, B, Acc};
  O.Ty = ResTy;
  Ops.push_back(std::move(O));
  return V;
}

void Function::createBarrier() {
  Op O;
  O.Kind = OpKind::Barrier;
  Ops.push_back(std::move(O));
}

void Function::createReturn(Value *V) {
  Op O;
  O.Kind = OpKind::Return;
  if (V)
    O.Operands = {V};
  Ops.push_back(std::move(O));
  if (V) {
    ResultTy = V->getType();
    HasResult = true;
  }
}

static StringRef binaryName(OpKind Kind) {
  switch (Kind) {
  case OpKind::AddI:
    return "arith.addi";
  case OpKind::SubI:
    return "arith.subi";
  case OpKind::MulI:
    return "arith.muli";
  case OpKind::DivI:
    return "arith.divsi";
  case OpKind::RemI:
    return "arith.remsi";
  case OpKind::AddF:
    return "arith.addf";
  case OpKind::SubF:
    return "arith.subf";
  case OpKind::MulF:
    return "arith.mulf";
  case OpKind::DivF:
    return "arith.divf";
  case OpKind::AndI:
    return "arith.andi";
  case OpKind::OrI:
    return "arith.ori";
  case OpKind::XOrI:
    return "arith.xori";
  case OpKind::ShlI:
    return "arith.shli";
  case OpKind::ShrI:
    return "arith.shrui";
  case OpKind::AShrI:
    return "arith.shrsi";
  default:
    llvm_unreachable("not a binary op");
  }
}

static StringRef cmpName(CmpPred Pred) {
  switch (Pred) {
  case CmpPred::Eq:
    return "eq";
  case CmpPred::Ne:
    return "ne";
  case CmpPred::Slt:
    return "slt";
  case CmpPred::Sle:
    return "sle";
  case CmpPred::Sgt:
    return "sgt";
  case CmpPred::Sge:
    return "sge";
  case CmpPred::Ult:
    return "ult";
  case CmpPred::Ule:
    return "ule";
  case CmpPred::Ugt:
    return "ugt";
  case CmpPred::Uge:
    return "uge";
  }
  llvm_unreachable("unknown cmp predicate");
}

static StringRef cmpfName(CmpPred Pred) {
  switch (Pred) {
  case CmpPred::Eq:
    return "oeq";
  case CmpPred::Ne:
    return "one";
  case CmpPred::Slt:
    return "olt";
  case CmpPred::Sle:
    return "ole";
  case CmpPred::Sgt:
    return "ogt";
  case CmpPred::Sge:
    return "oge";
  case CmpPred::Ult:
  case CmpPred::Ule:
  case CmpPred::Ugt:
  case CmpPred::Uge:
    llvm_unreachable("unsigned predicates do not exist for f32");
  }
  llvm_unreachable("unknown cmp predicate");
}

static void printOps(llvm::raw_ostream &OS, llvm::ArrayRef<Op> Ops,
                     unsigned Indent) {
  const std::string Pad(Indent, ' ');
  for (const Op &O : Ops) {
    OS << Pad;
    switch (O.Kind) {
    case OpKind::Constant:
      O.Result->printName(OS);
      OS << " = arith.constant ";
      if (O.Ty.isF32())
        OS << format("%.6e", O.FImm);
      else
        OS << O.Imm;
      OS << " : ";
      O.Ty.print(OS);
      OS << "\n";
      break;
    case OpKind::GetProgramId: {
      O.Result->printName(OS);
      OS << " = tt.get_program_id ";
      OS << (O.Axis == 0 ? "x" : O.Axis == 1 ? "y" : "z");
      OS << " : i32\n";
      break;
    }
    case OpKind::MakeRange:
      O.Result->printName(OS);
      OS << " = tt.make_range {start = " << O.Start << " : i32, end = "
         << O.End << " : i32} : ";
      O.Ty.print(OS);
      OS << "\n";
      break;
    case OpKind::ExpandDims:
      O.Result->printName(OS);
      OS << " = tt.expand_dims ";
      O.Operands[0]->printName(OS);
      OS << " {axis = " << O.Axis << " : i32} : ";
      O.Operands[0]->getType().print(OS);
      OS << " -> ";
      O.Result->getType().print(OS);
      OS << "\n";
      break;
    case OpKind::Broadcast:
      O.Result->printName(OS);
      OS << " = tt.broadcast ";
      O.Operands[0]->printName(OS);
      OS << " : ";
      O.Operands[0]->getType().print(OS);
      OS << " -> ";
      O.Result->getType().print(OS);
      OS << "\n";
      break;
    case OpKind::Trans:
      O.Result->printName(OS);
      OS << " = tt.trans ";
      O.Operands[0]->printName(OS);
      OS << " {order = array<i32: 1, 0>} : ";
      O.Operands[0]->getType().print(OS);
      OS << " -> ";
      O.Result->getType().print(OS);
      OS << "\n";
      break;
    case OpKind::Splat:
      O.Result->printName(OS);
      OS << " = tt.splat ";
      O.Operands[0]->printName(OS);
      OS << " : ";
      O.Operands[0]->getType().print(OS);
      OS << " -> ";
      O.Ty.print(OS);
      OS << "\n";
      break;
    case OpKind::AddPtr:
      O.Result->printName(OS);
      OS << " = tt.addptr ";
      O.Operands[0]->printName(OS);
      OS << ", ";
      O.Operands[1]->printName(OS);
      OS << " : ";
      O.Operands[0]->getType().print(OS);
      OS << ", ";
      O.Operands[1]->getType().print(OS);
      OS << "\n";
      break;
    case OpKind::Load:
      O.Result->printName(OS);
      OS << " = tt.load ";
      O.Operands[0]->printName(OS);
      if (O.Operands.size() > 1) {
        OS << ", ";
        O.Operands[1]->printName(OS);
      }
      OS << " : ";
      O.Ty.print(OS);
      OS << "\n";
      break;
    case OpKind::Barrier:
      OS << "ttg.barrier all";
      OS << "\n";
      break;
    case OpKind::Store:
      OS << "tt.store ";
      O.Operands[0]->printName(OS);
      OS << ", ";
      O.Operands[1]->printName(OS);
      if (O.Operands.size() > 2) {
        OS << ", ";
        O.Operands[2]->printName(OS);
      }
      OS << " : ";
      O.Ty.print(OS);
      OS << "\n";
      break;
    case OpKind::CmpI:
      O.Result->printName(OS);
      OS << " = arith.cmpi " << cmpName(O.Pred) << ", ";
      O.Operands[0]->printName(OS);
      OS << ", ";
      O.Operands[1]->printName(OS);
      OS << " : ";
      O.Ty.print(OS);
      OS << "\n";
      break;
    case OpKind::CmpF:
      O.Result->printName(OS);
      OS << " = arith.cmpf " << cmpfName(O.Pred) << ", ";
      O.Operands[0]->printName(OS);
      OS << ", ";
      O.Operands[1]->printName(OS);
      OS << " : ";
      O.Ty.print(OS);
      OS << "\n";
      break;
    case OpKind::Exp2:
      O.Result->printName(OS);
      OS << " = math.exp2 ";
      O.Operands[0]->printName(OS);
      OS << " : ";
      O.Ty.print(OS);
      OS << "\n";
      break;
    case OpKind::Reduce: {
      Type SrcTy = O.Operands[0]->getType();
      Type ElemTy = SrcTy.getElementType();
      unsigned ID = O.Result->getID();
      O.Result->printName(OS);
      OS << " = \"tt.reduce\"(";
      O.Operands[0]->printName(OS);
      OS << ") <{axis = " << O.Axis << " : i32}> ({\n      ^bb0(%red" << ID
         << "a: ";
      ElemTy.print(OS);
      OS << ", %red" << ID << "b: ";
      ElemTy.print(OS);
      OS << "):\n        %red" << ID << "v = arith.";
      if (O.Red == ReduceKind::Sum)
        OS << (ElemTy.isF32() ? "addf" : "addi");
      else
        OS << (ElemTy.isF32() ? "maximumf" : "maxsi");
      OS << " %red" << ID << "a, %red" << ID << "b : ";
      ElemTy.print(OS);
      OS << "\n        tt.reduce.return %red" << ID << "v : ";
      ElemTy.print(OS);
      OS << "\n      }) : (";
      SrcTy.print(OS);
      OS << ") -> ";
      O.Result->getType().print(OS);
      OS << "\n";
      break;
    }
    case OpKind::Select:
      O.Result->printName(OS);
      OS << " = arith.select ";
      O.Operands[0]->printName(OS);
      OS << ", ";
      O.Operands[1]->printName(OS);
      OS << ", ";
      O.Operands[2]->printName(OS);
      OS << " : ";
      // arith.select infers a scalar i1 condition from the result type.
      // A tensor/vector condition must be spelled out as
      // `: tensor<...xi1>, result-type` (ArithOps.cpp SelectOp::parse).
      if (!O.Operands[0]->getType().isI1()) {
        O.Operands[0]->getType().print(OS);
        OS << ", ";
      }
      O.Result->getType().print(OS);
      OS << "\n";
      break;
    case OpKind::Return:
      OS << "tt.return";
      if (!O.Operands.empty()) {
        OS << " ";
        O.Operands[0]->printName(OS);
        OS << " : ";
        O.Operands[0]->getType().print(OS);
      }
      OS << "\n";
      break;
    case OpKind::SharedAlloc:
      O.Result->printName(OS);
      OS << " = \"gpumin.shared_alloc\"() : () -> ";
      O.Ty.print(OS);
      OS << "\n";
      break;
    case OpKind::SharedStore:
      OS << "\"gpumin.shared_store\"(";
      O.Operands[0]->printName(OS);
      OS << ", ";
      O.Operands[1]->printName(OS);
      OS << ") : (";
      O.Operands[0]->getType().print(OS);
      OS << ", ";
      O.Operands[1]->getType().print(OS);
      OS << ") -> ()\n";
      break;
    case OpKind::SharedCol:
    case OpKind::SharedRow:
      O.Result->printName(OS);
      OS << (O.Kind == OpKind::SharedCol ? " = \"gpumin.shared_col\"("
                                         : " = \"gpumin.shared_row\"(");
      O.Operands[0]->printName(OS);
      OS << ") <{index = " << O.Imm << " : i32}> : (";
      O.Operands[0]->getType().print(OS);
      OS << ") -> ";
      O.Result->getType().print(OS);
      OS << "\n";
      break;
    case OpKind::Dot:
      O.Result->printName(OS);
      OS << " = tt.dot ";
      O.Operands[0]->printName(OS);
      OS << ", ";
      O.Operands[1]->printName(OS);
      OS << ", ";
      O.Operands[2]->printName(OS);
      OS << ", inputPrecision = tf32 : ";
      O.Operands[0]->getType().print(OS);
      OS << " * ";
      O.Operands[1]->getType().print(OS);
      OS << " -> ";
      O.Result->getType().print(OS);
      OS << "\n";
      break;
    case OpKind::ScfIf:
      OS << "scf.if ";
      O.Operands[0]->printName(OS);
      OS << " {\n";
      printOps(OS, O.ThenOps, Indent + 2);
      if (!O.ElseOps.empty()) {
        OS << Pad << "} else {\n";
        printOps(OS, O.ElseOps, Indent + 2);
      }
      OS << Pad << "}\n";
      break;
    default:
      O.Result->printName(OS);
      OS << " = " << binaryName(O.Kind) << " ";
      O.Operands[0]->printName(OS);
      OS << ", ";
      O.Operands[1]->printName(OS);
      OS << " : ";
      O.Ty.print(OS);
      OS << "\n";
      break;
    }
  }
}

void Function::print(raw_ostream &OS) const {
  OS << "  tt.func public @" << Name << "(";
  for (size_t I = 0; I < Args.size(); ++I) {
    if (I)
      OS << ", ";
    Args[I]->printName(OS);
    OS << ": ";
    Args[I]->getType().print(OS);
  }
  OS << ")";
  if (HasResult) {
    OS << " -> ";
    ResultTy.print(OS);
  }
  OS << " {\n";
  printOps(OS, Ops, 4);
  OS << "  }\n";
}

Function &Module::createFunction(StringRef Name) {
  Functions.emplace_back(Name);
  return Functions.back();
}

void Module::print(raw_ostream &OS) const {
  OS << "module {\n";
  for (const Function &F : Functions)
    F.print(OS);
  OS << "}\n";
}

} // namespace gpumintriton
