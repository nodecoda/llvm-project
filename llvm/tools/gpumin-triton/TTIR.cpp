//===-- TTIR.cpp - TTIR-min for gpumin-triton -----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "TTIR.h"

#include "llvm/Support/ErrorHandling.h"
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
  Value *V = createValue(Type::i1());
  Op O;
  O.Kind = OpKind::CmpI;
  O.Result = V;
  O.Operands = {LHS, RHS};
  O.Pred = Pred;
  O.Ty = LHS->getType();
  Ops.push_back(std::move(O));
  return V;
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

  for (const Op &O : Ops) {
    OS << "    ";
    switch (O.Kind) {
    case OpKind::Constant:
      O.Result->printName(OS);
      OS << " = arith.constant " << O.Imm << " : ";
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
