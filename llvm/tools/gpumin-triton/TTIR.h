//===-- TTIR.h - TTIR-min for gpumin-triton ---------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The textual subset is deliberately close to Triton's TTIR so that the
// chapter gates can hand it to triton-opt.  It is not an MLIR dialect; the
// C++ classes are the source of truth and the printer is the contract.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_GPUMIN_TRITON_TTIR_H
#define LLVM_TOOLS_GPUMIN_TRITON_TTIR_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"
#include <memory>
#include <string>
#include <vector>

namespace gpumintriton {

class Type {
public:
  enum class Kind { I1, I32, F32, Ptr, Tensor };

  Type() = default;
  static Type i1();
  static Type i32();
  static Type f32();
  static Type ptr(Type Pointee, unsigned AddressSpace = 1);
  static Type tensor(llvm::ArrayRef<int64_t> Shape, Type Element);

  Kind getKind() const { return K; }
  bool isI1() const { return K == Kind::I1; }
  bool isI32() const { return K == Kind::I32; }
  bool isF32() const { return K == Kind::F32; }
  bool isPtr() const { return K == Kind::Ptr; }
  bool isTensor() const { return K == Kind::Tensor; }

  Type getPointee() const;
  Type getElementType() const;
  llvm::ArrayRef<int64_t> getShape() const { return Shape; }
  unsigned getAddressSpace() const { return AddressSpace; }

  void print(llvm::raw_ostream &OS) const;
  bool operator==(const Type &Other) const;
  bool operator!=(const Type &Other) const { return !(*this == Other); }

private:
  Kind K = Kind::I32;
  unsigned AddressSpace = 0;
  std::shared_ptr<Type> Elem;
  llvm::SmallVector<int64_t, 2> Shape;
};

class Value {
public:
  unsigned getID() const { return ID; }
  Type getType() const { return Ty; }
  llvm::StringRef getName() const { return Name; }
  void setName(llvm::StringRef N) { Name = N.str(); }
  // Chapter 34: an alloc_shared result is a handle, not a value tile.  The
  // textual type stays a tensor so the TTIR remains parseable, but the
  // frontend refuses to use a handle in arithmetic.
  bool isShared() const { return Shared; }
  void markShared() { Shared = true; }
  void printName(llvm::raw_ostream &OS) const;

private:
  friend class Function;
  Value(unsigned ID, Type Ty) : ID(ID), Ty(Ty) {}

  unsigned ID;
  Type Ty;
  std::string Name;
  bool Shared = false;
};

enum class OpKind {
  Constant,
  AddI,
  SubI,
  MulI,
  DivI,
  RemI,
  AddF,
  SubF,
  MulF,
  DivF,
  Exp2,
  AndI,
  OrI,
  XOrI,
  ShlI,
  ShrI,
  AShrI,
  CmpI,
  CmpF,
  GetProgramId,
  MakeRange,
  ExpandDims,
  Broadcast,
  Trans,
  Splat,
  AddPtr,
  Load,
  Store,
  Reduce,
  Select,
  Barrier,
  ScfIf,
  SharedAlloc,
  SharedStore,
  SharedCol,
  SharedRow,
  Dot,
  Return
};

enum class ReduceKind { Sum, Max };

enum class CmpPred { Eq, Ne, Slt, Sle, Sgt, Sge, Ult, Ule, Ugt, Uge };

struct Op {
  OpKind Kind = OpKind::Constant;
  Value *Result = nullptr;
  llvm::SmallVector<Value *, 2> Operands;
  int64_t Imm = 0;
  double FImm = 0.0;
  int64_t Start = 0;
  int64_t End = 0;
  unsigned Axis = 0;
  ReduceKind Red = ReduceKind::Sum;
  CmpPred Pred = CmpPred::Eq;
  Type Ty;
  // Chapter 33: scf.if carries one or two nested op lists.
  std::vector<Op> ThenOps;
  std::vector<Op> ElseOps;
};

class Function {
public:
  explicit Function(llvm::StringRef Name) : Name(Name.str()) {}
  Function(const Function &) = delete;
  Function &operator=(const Function &) = delete;
  Function(Function &&) noexcept = default;
  Function &operator=(Function &&) noexcept = default;

  Value *addArg(llvm::StringRef Name, Type Ty);
  Value *createConstant(int64_t Imm, Type Ty);
  Value *createConstant(double FImm, Type Ty);
  Value *createBinary(OpKind Kind, Value *LHS, Value *RHS);
  Value *createCmp(CmpPred Pred, Value *LHS, Value *RHS);
  Value *createGetProgramId(unsigned Axis);
  Value *createMakeRange(int64_t Start, int64_t End);
  Value *createExpandDims(Value *Src, unsigned Axis);
  Value *createBroadcast(Value *Src, Type DstTy);
  Value *createTrans(Value *Src);
  Value *createSplat(Value *Scalar, Type TensorTy);
  Value *createAddPtr(Value *Ptr, Value *Offset);
  Value *createLoad(Value *Ptr, Value *Mask);
  Value *createReduce(ReduceKind Kind, Value *Src, unsigned Axis);
  Value *createExp2(Value *Src);
  Value *createSelect(Value *Cond, Value *TrueV, Value *FalseV);
  void createStore(Value *Ptr, Value *Stored, Value *Mask);
  void createBarrier();
  void createScfIf(Value *Cond, std::vector<Op> ThenOps,
                   std::vector<Op> ElseOps);
  Value *createSharedAlloc(Type TensorTy);
  void createSharedStore(Value *Handle, Value *Src);
  Value *createSharedCol(Value *Handle, int64_t Index);
  Value *createSharedRow(Value *Handle, int64_t Index);
  Value *createDot(Value *A, Value *B, Value *Acc);
  void createReturn(Value *V);
  void setResultType(Type Ty) { ResultTy = Ty; HasResult = true; }

  llvm::StringRef getName() const { return Name; }
  bool hasResult() const { return HasResult; }
  Type getResultType() const { return ResultTy; }
  llvm::ArrayRef<Value *> getArgs() const { return Args; }
  llvm::ArrayRef<Op> getOps() const { return Ops; }
  size_t getOpCount() const { return Ops.size(); }
  std::vector<Op> takeOpsFrom(size_t Mark);

  void print(llvm::raw_ostream &OS) const;

private:
  Value *createValue(Type Ty);

  std::string Name;
  unsigned NextID = 0;
  std::vector<std::unique_ptr<Value>> Values;
  std::vector<Value *> Args;
  std::vector<Op> Ops;
  Type ResultTy;
  bool HasResult = false;
};

class Module {
public:
  Function &createFunction(llvm::StringRef Name);
  llvm::ArrayRef<Function> getFunctions() const { return Functions; }
  void print(llvm::raw_ostream &OS) const;

private:
  std::vector<Function> Functions;
};

} // namespace gpumintriton

#endif // LLVM_TOOLS_GPUMIN_TRITON_TTIR_H
