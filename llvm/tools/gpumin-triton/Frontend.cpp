//===-- Frontend.cpp - Triton Python subset frontend ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Frontend.h"

#include "llvm/Support/Error.h"
#include <map>

using namespace llvm;
using namespace llvm::json;

namespace gpumintriton {

namespace {

StringRef typeOf(const Object &Node) {
  if (auto S = Node.getString("_type"))
    return *S;
  return {};
}

StringRef stringField(const Object &Node, StringRef Name) {
  if (auto S = Node.getString(Name))
    return *S;
  return {};
}

bool isJitDecorator(const json::Value &Decorator) {
  const Object *Node = Decorator.getAsObject();
  if (!Node)
    return false;
  StringRef Type = typeOf(*Node);
  if (Type == "Name")
    return stringField(*Node, "id") == "jit";
  if (Type == "Attribute")
    return stringField(*Node, "attr") == "jit";
  if (Type == "Call") {
    if (const json::Value *Func = Node->get("func"))
      return isJitDecorator(*Func);
  }
  return false;
}

bool isConstexprAnnotation(const json::Value &Annotation) {
  const Object *Node = Annotation.getAsObject();
  if (!Node)
    return false;
  StringRef Type = typeOf(*Node);
  if (Type == "Name")
    return stringField(*Node, "id") == "constexpr";
  if (Type == "Attribute")
    return stringField(*Node, "attr") == "constexpr";
  return false;
}

void appendArgList(const Object &ArgsObj, StringRef Field,
                   std::vector<KernelArg> &Out) {
  const Array *Args = ArgsObj.getArray(Field);
  if (!Args)
    return;
  for (const json::Value &Item : *Args) {
    const Object *Arg = Item.getAsObject();
    if (!Arg || typeOf(*Arg) != "arg")
      continue;
    KernelArg A;
    A.Name = stringField(*Arg, "arg").str();
    A.Line = static_cast<unsigned>(Arg->getInteger("lineno").value_or(0));
    if (const json::Value *Annotation = Arg->get("annotation"))
      A.IsConstexpr = isConstexprAnnotation(*Annotation);
    Out.push_back(std::move(A));
  }
}

unsigned lineOf(const Object &Node) {
  return static_cast<unsigned>(Node.getInteger("lineno").value_or(0));
}

unsigned columnOf(const Object &Node) {
  return static_cast<unsigned>(Node.getInteger("col_offset").value_or(0));
}

bool isPtrLike(const Type &T) {
  return T.isPtr() || (T.isTensor() && T.getElementType().isPtr());
}

bool isI32Tensor(const Type &T) {
  return T.isTensor() && T.getElementType().isI32();
}

bool isFloatLike(const Type &T) {
  return T.isF32() || (T.isTensor() && T.getElementType().isF32());
}

bool isF32Tensor(const Type &T) {
  return T.isTensor() && T.getElementType().isF32();
}

/// Lowers one kernel body into one TTIR function.
///
/// Chapter 22 handled scalars; chapter 23 adds the tensor and pointer
/// semantics needed by tl.program_id / tl.arange / tl.load / tl.store.
class Lowerer {
public:
  Lowerer(const PyAst &Ast, Function &F, const KernelBindings &Bindings)
      : Ast(Ast), F(F), Bindings(Bindings) {}

  Error lower(const Object &Fn);

private:
  Error errAt(const Object &Node, const Twine &Msg) const;
  Error lowerStmt(const Object &Node);
  Expected<Value *> lowerExpr(const json::Value &Node);
  Expected<Value *> lowerExpr(const Object &Node);
  Expected<Value *> lowerCall(const Object &Node);
  Expected<Value *> lowerBinary(const Object &Node, OpKind Kind, Value *LHS,
                                Value *RHS);
  Expected<Value *> lowerCompare(const Object &Node, CmpPred Pred, Value *LHS,
                                 Value *RHS);
  Expected<int64_t> evalInt(const json::Value &Node);
  Value *splatTo(Value *V, Type TensorTy);
  void bindName(const Object &Target, Value *V);

  const PyAst &Ast;
  Function &F;
  const KernelBindings &Bindings;
  std::map<std::string, Value *> Env;
  std::map<std::string, int64_t> ConstValues;
  std::map<std::string, unsigned> NameCount;
  bool SawReturn = false;
};

Error Lowerer::errAt(const Object &Node, const Twine &Msg) const {
  return createStringError(inconvertibleErrorCode(), "%s:%u:%u: %s",
                           Ast.sourcePath().str().c_str(), lineOf(Node),
                           columnOf(Node), Msg.str().c_str());
}

Value *Lowerer::splatTo(Value *V, Type TensorTy) {
  if (V->getType() == TensorTy)
    return V;
  return F.createSplat(V, TensorTy);
}

void Lowerer::bindName(const Object &Target, Value *V) {
  std::string Name = stringField(Target, "id").str();
  Env[Name] = V;
  if (V->getName().empty()) {
    unsigned Count = NameCount[Name]++;
    if (Count == 0)
      V->setName(Name);
    else
      V->setName(Name + "_" + std::to_string(Count));
  }
}

Error Lowerer::lowerStmt(const Object &Node) {
  StringRef Type = typeOf(Node);

  if (Type == "Pass") {
    return Error::success();
  }

  if (Type == "Assign") {
    const Array *Targets = Node.getArray("targets");
    const json::Value *ValueNode = Node.get("value");
    if (!Targets || !ValueNode)
      return errAt(Node, "malformed Assign");
    if (Targets->size() != 1)
      return errAt(Node, "multiple assignment targets are not supported");
    const Object *Target = (*Targets)[0].getAsObject();
    if (!Target || typeOf(*Target) != "Name")
      return errAt(Node, "only simple name assignment is supported");

    Expected<Value *> ValueOrErr = lowerExpr(*ValueNode);
    if (!ValueOrErr)
      return ValueOrErr.takeError();
    if (!*ValueOrErr)
      return errAt(Node, "the right-hand side does not produce a value");
    bindName(*Target, *ValueOrErr);
    return Error::success();
  }

  if (Type == "Return") {
    if (const json::Value *ValueNode = Node.get("value")) {
      Expected<Value *> ValueOrErr = lowerExpr(*ValueNode);
      if (!ValueOrErr)
        return ValueOrErr.takeError();
      Value *V = *ValueOrErr;
      if (!V)
        return errAt(Node, "the returned expression does not produce a value");
      if (SawReturn && F.getResultType() != V->getType())
        return errAt(Node, "return type mismatch: earlier return had a "
                           "different type");
      F.createReturn(V);
    } else {
      F.createReturn(nullptr);
    }
    SawReturn = true;
    return Error::success();
  }

  if (Type == "Expr") {
    const json::Value *ValueNode = Node.get("value");
    if (!ValueNode)
      return errAt(Node, "malformed Expr");
    // A docstring is a string constant expression; skip it.
    if (const Object *ExprObj = ValueNode->getAsObject()) {
      if (typeOf(*ExprObj) == "Constant" &&
          ExprObj->getString("value").has_value())
        return Error::success();
    }
    Expected<Value *> ValueOrErr = lowerExpr(*ValueNode);
    if (!ValueOrErr)
      return ValueOrErr.takeError();
    return Error::success();
  }

  return errAt(Node, ("unsupported statement '" + Type + "'").str());
}

Expected<Value *> Lowerer::lowerExpr(const json::Value &Node) {
  const Object *Obj = Node.getAsObject();
  if (!Obj)
    return createStringError(inconvertibleErrorCode(),
                             "%s: malformed expression node",
                             Ast.sourcePath().str().c_str());
  return lowerExpr(*Obj);
}

Expected<Value *> Lowerer::lowerExpr(const Object &Node) {
  StringRef Type = typeOf(Node);

  if (Type == "Constant") {
    // llvm::json::getAsInteger() converts an integral double such as 2.0 to
    // int64, so the dumper's explicit marker decides the TTIR type.
    if (Node.getBoolean("value_is_float").value_or(false)) {
      if (auto FImm = Node.getNumber("value"))
        return F.createConstant(*FImm, Type::f32());
      return errAt(Node, "malformed float constant");
    }
    if (auto Imm = Node.getInteger("value"))
      return F.createConstant(*Imm, Type::i32());
    return errAt(Node, "only integer and float constants are supported");
  }

  if (Type == "Name") {
    std::string Name = stringField(Node, "id").str();
    auto It = Env.find(Name);
    if (It == Env.end())
      return errAt(Node, "unknown name '" + Name + "'");
    return It->second;
  }

  if (Type == "BinOp") {
    const Object *OpObj = Node.getObject("op");
    const json::Value *LHSNode = Node.get("left");
    const json::Value *RHSNode = Node.get("right");
    if (!OpObj || !LHSNode || !RHSNode)
      return errAt(Node, "malformed BinOp");

    OpKind Kind;
    StringRef OpType = typeOf(*OpObj);
    if (OpType == "Add")
      Kind = OpKind::AddI;
    else if (OpType == "Sub")
      Kind = OpKind::SubI;
    else if (OpType == "Mult")
      Kind = OpKind::MulI;
    else if (OpType == "BitAnd")
      Kind = OpKind::AndI;
    else if (OpType == "BitOr")
      Kind = OpKind::OrI;
    else if (OpType == "BitXor")
      Kind = OpKind::XOrI;
    else if (OpType == "LShift")
      Kind = OpKind::ShlI;
    else if (OpType == "RShift")
      Kind = OpKind::AShrI; // Python's >> is arithmetic for signed ints.
    else
      return errAt(Node,
                   ("unsupported binary operator '" + OpType + "'").str());

    Expected<Value *> LHSOrErr = lowerExpr(*LHSNode);
    if (!LHSOrErr)
      return LHSOrErr.takeError();
    Expected<Value *> RHSOrErr = lowerExpr(*RHSNode);
    if (!RHSOrErr)
      return RHSOrErr.takeError();
    return lowerBinary(Node, Kind, *LHSOrErr, *RHSOrErr);
  }

  if (Type == "Compare") {
    const Array *Ops = Node.getArray("ops");
    const json::Value *LHSNode = Node.get("left");
    const Array *Comparators = Node.getArray("comparators");
    if (!Ops || Ops->size() != 1 || !LHSNode || !Comparators ||
        Comparators->size() != 1)
      return errAt(Node, "chained comparisons are not supported");

    const Object *OpObj = (*Ops)[0].getAsObject();
    if (!OpObj)
      return errAt(Node, "malformed comparison operator");
    CmpPred Pred;
    StringRef OpType = typeOf(*OpObj);
    if (OpType == "Eq")
      Pred = CmpPred::Eq;
    else if (OpType == "NotEq")
      Pred = CmpPred::Ne;
    else if (OpType == "Lt")
      Pred = CmpPred::Slt;
    else if (OpType == "LtE")
      Pred = CmpPred::Sle;
    else if (OpType == "Gt")
      Pred = CmpPred::Sgt;
    else if (OpType == "GtE")
      Pred = CmpPred::Sge;
    else
      return errAt(Node, ("unsupported comparison '" + OpType + "'").str());

    Expected<Value *> LHSOrErr = lowerExpr(*LHSNode);
    if (!LHSOrErr)
      return LHSOrErr.takeError();
    Expected<Value *> RHSOrErr = lowerExpr((*Comparators)[0]);
    if (!RHSOrErr)
      return RHSOrErr.takeError();
    return lowerCompare(Node, Pred, *LHSOrErr, *RHSOrErr);
  }

  if (Type == "UnaryOp") {
    const Object *OpObj = Node.getObject("op");
    const json::Value *OperandNode = Node.get("operand");
    if (!OpObj || !OperandNode)
      return errAt(Node, "malformed UnaryOp");
    StringRef OpType = typeOf(*OpObj);
    if (OpType == "UAdd")
      return lowerExpr(*OperandNode);
    if (OpType == "USub") {
      Expected<Value *> OperandOrErr = lowerExpr(*OperandNode);
      if (!OperandOrErr)
        return OperandOrErr.takeError();
      Value *Operand = *OperandOrErr;
      gpumintriton::Type OpTy = Operand->getType();
      if (OpTy.isF32()) {
        Value *Zero = F.createConstant(0.0, gpumintriton::Type::f32());
        return F.createBinary(OpKind::SubF, Zero, Operand);
      }
      if (isF32Tensor(OpTy)) {
        gpumintriton::Type TensorTy = OpTy;
        Value *Zero = F.createConstant(0.0, gpumintriton::Type::f32());
        return F.createBinary(OpKind::SubF, splatTo(Zero, TensorTy),
                              Operand);
      }
      if (!OpTy.isI32())
        return errAt(Node, "unary minus requires an i32 or f32 operand");
      Value *Zero = F.createConstant(int64_t(0), gpumintriton::Type::i32());
      return F.createBinary(OpKind::SubI, Zero, Operand);
    }
    return errAt(Node, ("unsupported unary operator '" + OpType + "'").str());
  }

  if (Type == "Call")
    return lowerCall(Node);

  return errAt(Node, ("unsupported expression '" + Type + "'").str());
}

Expected<Value *> Lowerer::lowerBinary(const Object &Node, OpKind Kind,
                                       Value *LHS, Value *RHS) {
  Type LT = LHS->getType();
  Type RT = RHS->getType();

  if (isPtrLike(LT) || isPtrLike(RT)) {
    if (Kind != OpKind::AddI)
      return errAt(Node, "pointer arithmetic only supports '+'");
    Value *Ptr = LHS;
    Value *Offset = RHS;
    Type PtrTy = LT;
    Type OffsetTy = RT;
    if (isPtrLike(RT)) {
      if (isPtrLike(LT))
        return errAt(Node, "cannot add two pointers");
      std::swap(Ptr, Offset);
      std::swap(PtrTy, OffsetTy);
    }
    if (!(OffsetTy.isI32() || isI32Tensor(OffsetTy)))
      return errAt(Node, "pointer offset must be i32 or tensor<i32>");
    if (PtrTy.isTensor() && OffsetTy.isI32()) {
      Offset = F.createSplat(
          Offset, Type::tensor(PtrTy.getShape(), Type::i32()));
    } else if (PtrTy.isPtr() && OffsetTy.isTensor()) {
      Ptr = F.createSplat(Ptr, Type::tensor(OffsetTy.getShape(), PtrTy));
    } else if (PtrTy.isTensor() && OffsetTy.isTensor()) {
      if (PtrTy.getShape() != OffsetTy.getShape())
        return errAt(Node, "pointer/offset tensor shapes differ");
    }
    return F.createAddPtr(Ptr, Offset);
  }

  if (isFloatLike(LT) || isFloatLike(RT)) {
    OpKind FloatKind;
    switch (Kind) {
    case OpKind::AddI:
      FloatKind = OpKind::AddF;
      break;
    case OpKind::SubI:
      FloatKind = OpKind::SubF;
      break;
    case OpKind::MulI:
      FloatKind = OpKind::MulF;
      break;
    default:
      return errAt(Node,
                   "floating-point operands only support '+', '-' and '*'");
    }
    if (LT.isF32() && RT.isF32())
      return F.createBinary(FloatKind, LHS, RHS);
    if (LT.isTensor() && RT.isF32()) {
      if (!LT.getElementType().isF32())
        return errAt(Node, "mixed f32/tensor<f32> operand types");
      return F.createBinary(FloatKind, LHS, splatTo(RHS, LT));
    }
    if (LT.isF32() && RT.isTensor()) {
      if (!RT.getElementType().isF32())
        return errAt(Node, "mixed f32/tensor<f32> operand types");
      return F.createBinary(FloatKind, splatTo(LHS, RT), RHS);
    }
    if (LT.isTensor() && RT.isTensor()) {
      if (LT != RT)
        return errAt(Node, "tensor type mismatch");
      return F.createBinary(FloatKind, LHS, RHS);
    }
    return errAt(Node, "mixed float and integer operands are not supported");
  }

  if (LT.isI32() && RT.isI32())
    return F.createBinary(Kind, LHS, RHS);
  if (LT.isTensor() && RT.isI32())
    return F.createBinary(Kind, LHS, splatTo(RHS, LT));
  if (LT.isI32() && RT.isTensor())
    return F.createBinary(Kind, splatTo(LHS, RT), RHS);
  if (LT.isTensor() && RT.isTensor()) {
    if (LT != RT)
      return errAt(Node, "tensor type mismatch");
    return F.createBinary(Kind, LHS, RHS);
  }
  return errAt(Node, "unsupported operand types for this operator");
}

Expected<Value *> Lowerer::lowerCompare(const Object &Node, CmpPred Pred,
                                        Value *LHS, Value *RHS) {
  Type LT = LHS->getType();
  Type RT = RHS->getType();

  if (isFloatLike(LT) || isFloatLike(RT)) {
    if (LT.isF32() && RT.isF32())
      return F.createCmp(Pred, LHS, RHS);
    if (LT.isTensor() && RT.isF32()) {
      if (!LT.getElementType().isF32())
        return errAt(Node, "mixed f32/tensor<f32> operands in comparison");
      return F.createCmp(Pred, LHS, splatTo(RHS, LT));
    }
    if (LT.isF32() && RT.isTensor()) {
      if (!RT.getElementType().isF32())
        return errAt(Node, "mixed f32/tensor<f32> operands in comparison");
      return F.createCmp(Pred, splatTo(LHS, RT), RHS);
    }
    if (LT.isTensor() && RT.isTensor()) {
      if (LT != RT)
        return errAt(Node, "tensor type mismatch in comparison");
      return F.createCmp(Pred, LHS, RHS);
    }
    return errAt(Node,
                 "mixed float and integer operands in comparison");
  }

  if (LT.isI32() && RT.isI32())
    return F.createCmp(Pred, LHS, RHS);
  if (LT.isTensor() && RT.isI32())
    return F.createCmp(Pred, LHS, splatTo(RHS, LT));
  if (LT.isI32() && RT.isTensor())
    return F.createCmp(Pred, splatTo(LHS, RT), RHS);
  if (LT.isTensor() && RT.isTensor()) {
    if (LT != RT)
      return errAt(Node, "tensor type mismatch in comparison");
    return F.createCmp(Pred, LHS, RHS);
  }
  return errAt(Node, "comparisons require i32 or tensor<i32> operands");
}

Expected<int64_t> Lowerer::evalInt(const json::Value &NodeValue) {
  const Object *Node = NodeValue.getAsObject();
  if (!Node)
    return createStringError(inconvertibleErrorCode(),
                             "%s: malformed constant expression",
                             Ast.sourcePath().str().c_str());
  StringRef Type = typeOf(*Node);
  if (Type == "Constant") {
    if (auto Imm = Node->getInteger("value"))
      return *Imm;
    return errAt(*Node, "expected an integer constant");
  }
  if (Type == "Name") {
    std::string Name = stringField(*Node, "id").str();
    auto It = ConstValues.find(Name);
    if (It != ConstValues.end())
      return It->second;
    return errAt(*Node, "'" + Name + "' is not a compile-time integer");
  }
  if (Type == "UnaryOp") {
    const Object *Op = Node->getObject("op");
    const json::Value *Operand = Node->get("operand");
    if (Op && Operand && typeOf(*Op) == "USub") {
      auto VOrErr = evalInt(*Operand);
      if (!VOrErr)
        return VOrErr.takeError();
      return -*VOrErr;
    }
  }
  return errAt(*Node, "expected a compile-time integer");
}

Expected<Value *> Lowerer::lowerCall(const Object &Node) {
  const Object *Func = Node.getObject("func");
  if (!Func || typeOf(*Func) != "Attribute")
    return errAt(Node, "only tl.* calls are supported in chapter 23");

  const Object *Receiver = Func->getObject("value");
  bool IsTL = Receiver && typeOf(*Receiver) == "Name" &&
              stringField(*Receiver, "id") == "tl";
  if (!IsTL)
    return errAt(Node, "only tl.* calls are supported in chapter 23");

  StringRef Name = stringField(*Func, "attr");
  const Array *Args = Node.getArray("args");
  std::map<std::string, const json::Value *> Keywords;
  if (const Array *KeywordNodes = Node.getArray("keywords")) {
    for (const json::Value &Item : *KeywordNodes) {
      const Object *Keyword = Item.getAsObject();
      if (!Keyword)
        return errAt(Node, "malformed keyword argument");
      auto ArgName = Keyword->getString("arg");
      if (!ArgName)
        return errAt(*Keyword, "**kwargs are not supported");
      Keywords[ArgName->str()] = Keyword->get("value");
    }
  }

  auto GetArg = [&](size_t I) -> const json::Value * {
    if (!Args || I >= Args->size())
      return nullptr;
    return &(*Args)[I];
  };

  if (Name == "program_id") {
    const json::Value *AxisNode = GetArg(0);
    if (!AxisNode)
      return errAt(Node, "tl.program_id expects one argument");
    Expected<int64_t> AxisOrErr = evalInt(*AxisNode);
    if (!AxisOrErr)
      return AxisOrErr.takeError();
    if (*AxisOrErr != 0)
      return errAt(Node, "chapter 23 only supports tl.program_id(0)");
    return F.createGetProgramId(0);
  }

  if (Name == "arange") {
    const json::Value *StartNode = GetArg(0);
    const json::Value *EndNode = GetArg(1);
    if (!StartNode || !EndNode)
      return errAt(Node, "tl.arange expects (start, end)");
    Expected<int64_t> StartOrErr = evalInt(*StartNode);
    if (!StartOrErr)
      return StartOrErr.takeError();
    Expected<int64_t> EndOrErr = evalInt(*EndNode);
    if (!EndOrErr)
      return EndOrErr.takeError();
    if (*EndOrErr <= *StartOrErr)
      return errAt(Node, "tl.arange requires end > start");
    return F.createMakeRange(*StartOrErr, *EndOrErr);
  }

  if (Name == "load") {
    const json::Value *PtrNode = GetArg(0);
    if (!PtrNode)
      return errAt(Node, "tl.load expects a pointer");
    Expected<Value *> PtrOrErr = lowerExpr(*PtrNode);
    if (!PtrOrErr)
      return PtrOrErr.takeError();
    Value *Ptr = *PtrOrErr;
    if (!Ptr->getType().isPtr() &&
        !(Ptr->getType().isTensor() &&
          Ptr->getType().getElementType().isPtr()))
      return errAt(Node, "tl.load expects a pointer or pointer tensor");
    if (Keywords.count("other"))
      return errAt(Node, "tl.load(other=...) arrives in a later chapter");

    Value *Mask = nullptr;
    auto MaskIt = Keywords.find("mask");
    if (MaskIt != Keywords.end()) {
      Expected<Value *> MaskOrErr = lowerExpr(*MaskIt->second);
      if (!MaskOrErr)
        return MaskOrErr.takeError();
      Mask = *MaskOrErr;
      if (Ptr->getType().isPtr()) {
        if (!Mask->getType().isI1())
          return errAt(Node, "a scalar load needs a scalar i1 mask");
      } else if (Mask->getType().isI1()) {
        Mask = F.createSplat(
            Mask, Type::tensor(Ptr->getType().getShape(), Type::i1()));
      } else if (!Mask->getType().isTensor() ||
                 !Mask->getType().getElementType().isI1()) {
        return errAt(Node, "tl.load mask must be i1 or tensor<i1>");
      } else if (Mask->getType().getShape() != Ptr->getType().getShape()) {
        return errAt(Node, "tl.load mask shape does not match the pointer");
      }
    }
    return F.createLoad(Ptr, Mask);
  }

  if (Name == "store") {
    const json::Value *PtrNode = GetArg(0);
    const json::Value *ValueNode = GetArg(1);
    if (!PtrNode || !ValueNode)
      return errAt(Node, "tl.store expects (pointer, value)");
    Expected<Value *> PtrOrErr = lowerExpr(*PtrNode);
    if (!PtrOrErr)
      return PtrOrErr.takeError();
    Expected<Value *> ValueOrErr = lowerExpr(*ValueNode);
    if (!ValueOrErr)
      return ValueOrErr.takeError();
    Value *Ptr = *PtrOrErr;
    Value *Stored = *ValueOrErr;
    if (!Ptr->getType().isPtr() &&
        !(Ptr->getType().isTensor() &&
          Ptr->getType().getElementType().isPtr()))
      return errAt(Node, "tl.store expects a pointer or pointer tensor");

    Value *Mask = nullptr;
    auto MaskIt = Keywords.find("mask");
    if (MaskIt != Keywords.end()) {
      Expected<Value *> MaskOrErr = lowerExpr(*MaskIt->second);
      if (!MaskOrErr)
        return MaskOrErr.takeError();
      Mask = *MaskOrErr;
      if (Ptr->getType().isPtr()) {
        if (!Mask->getType().isI1())
          return errAt(Node, "a scalar store needs a scalar i1 mask");
      } else if (Mask->getType().isI1()) {
        Mask = F.createSplat(
            Mask, Type::tensor(Ptr->getType().getShape(), Type::i1()));
      } else if (!Mask->getType().isTensor() ||
                 !Mask->getType().getElementType().isI1()) {
        return errAt(Node, "tl.store mask must be i1 or tensor<i1>");
      } else if (Mask->getType().getShape() != Ptr->getType().getShape()) {
        return errAt(Node, "tl.store mask shape does not match the pointer");
      }
    }
    F.createStore(Ptr, Stored, Mask);
    return nullptr;
  }

  if (Name == "sum" || Name == "max") {
    const json::Value *SrcNode = GetArg(0);
    if (!SrcNode)
      return errAt(Node, "tl." + Name + " expects a tensor");
    Expected<Value *> SrcOrErr = lowerExpr(*SrcNode);
    if (!SrcOrErr)
      return SrcOrErr.takeError();
    Value *Src = *SrcOrErr;
    if (!Src->getType().isTensor())
      return errAt(Node, "tl." + Name + " expects a tensor");

    const json::Value *AxisNode = GetArg(1);
    auto AxisIt = Keywords.find("axis");
    if (AxisIt != Keywords.end())
      AxisNode = AxisIt->second;
    int64_t Axis = 0;
    if (AxisNode) {
      Expected<int64_t> AxisOrErr = evalInt(*AxisNode);
      if (!AxisOrErr)
        return AxisOrErr.takeError();
      Axis = *AxisOrErr;
    }
    if (Axis != 0)
      return errAt(Node, "chapter 26 only reduces axis 0");

    Type ElemTy = Src->getType().getElementType();
    if (!ElemTy.isI32() && !ElemTy.isF32())
      return errAt(Node, "reductions require i32 or f32 elements");
    if (Name == "max") {
      if (ElemTy.isF32())
        return errAt(Node,
                     "tl.max on f32 arrives in chapter 28 (no fsel yet)");
      return F.createReduce(ReduceKind::Max, Src, 0);
    }
    return F.createReduce(ReduceKind::Sum, Src, 0);
  }

  return errAt(Node, ("unsupported tl function '" + Name + "'").str());
}

Error Lowerer::lower(const Object &Fn) {
  const Object *ArgsObj = Fn.getObject("args");
  if (!ArgsObj)
    return errAt(Fn, "kernel has no argument list");

  std::vector<KernelArg> ParsedArgs;
  appendArgList(*ArgsObj, "posonlyargs", ParsedArgs);
  appendArgList(*ArgsObj, "args", ParsedArgs);
  appendArgList(*ArgsObj, "kwonlyargs", ParsedArgs);

  for (const KernelArg &Arg : ParsedArgs) {
    NameCount[Arg.Name] = 1; // arguments already occupy the base name
    if (Arg.IsConstexpr) {
      auto It = Bindings.Constexprs.find(Arg.Name);
      if (It == Bindings.Constexprs.end())
        return errAt(Fn, "constexpr argument '" + Arg.Name +
                             "' has no value; pass --constexpr " + Arg.Name +
                             "=<int>");
      ConstValues[Arg.Name] = It->second;
      // Triton materialises constexpr parameters at function entry.
      Env[Arg.Name] = F.createConstant(It->second, Type::i32());
      continue;
    }
    Type Ty = Type::i32();
    auto It = Bindings.ArgTypes.find(Arg.Name);
    if (It != Bindings.ArgTypes.end())
      Ty = It->second;
    Env[Arg.Name] = F.addArg(Arg.Name, Ty);
  }
  if (ArgsObj->getObject("vararg") || ArgsObj->getObject("kwarg"))
    return errAt(Fn, "*args/**kwargs are not supported");

  const Array *Body = Fn.getArray("body");
  if (!Body)
    return errAt(Fn, "kernel has no body");

  for (const json::Value &Stmt : *Body) {
    const Object *StmtObj = Stmt.getAsObject();
    if (!StmtObj)
      return errAt(Fn, "malformed statement in kernel body");
    if (Error E = lowerStmt(*StmtObj))
      return E;
  }

  if (!SawReturn)
    F.createReturn(nullptr);
  return Error::success();
}

} // namespace

llvm::Expected<std::vector<KernelInfo>> Frontend::parseKernels() {
  const Object &Root = Ast.root();
  const Array *Body = Root.getArray("body");
  if (!Body)
    return createStringError(inconvertibleErrorCode(),
                             "'%s' has no module body",
                             Ast.sourcePath().str().c_str());

  std::vector<KernelInfo> Kernels;
  for (const json::Value &Item : *Body) {
    const Object *Fn = Item.getAsObject();
    if (!Fn || typeOf(*Fn) != "FunctionDef")
      continue;

    bool IsKernel = false;
    if (const Array *Decorators = Fn->getArray("decorator_list")) {
      for (const json::Value &Decorator : *Decorators) {
        if (isJitDecorator(Decorator)) {
          IsKernel = true;
          break;
        }
      }
    }
    if (!IsKernel)
      continue;

    KernelInfo Info;
    Info.Name = stringField(*Fn, "name").str();
    Info.Line = static_cast<unsigned>(Fn->getInteger("lineno").value_or(0));

    const Object *ArgsObj = Fn->getObject("args");
    if (!ArgsObj)
      return createStringError(inconvertibleErrorCode(),
                               "kernel '%s' has no argument list",
                               Info.Name.c_str());

    appendArgList(*ArgsObj, "posonlyargs", Info.Args);
    appendArgList(*ArgsObj, "args", Info.Args);
    appendArgList(*ArgsObj, "kwonlyargs", Info.Args);

    if (const Object *Vararg = ArgsObj->getObject("vararg")) {
      KernelArg A;
      A.Name = stringField(*Vararg, "arg").str();
      A.Line = static_cast<unsigned>(Vararg->getInteger("lineno").value_or(0));
      Info.Args.push_back(std::move(A));
    }
    if (const Object *Kwarg = ArgsObj->getObject("kwarg")) {
      KernelArg A;
      A.Name = stringField(*Kwarg, "arg").str();
      A.Line = static_cast<unsigned>(Kwarg->getInteger("lineno").value_or(0));
      Info.Args.push_back(std::move(A));
    }

    Kernels.push_back(std::move(Info));
  }

  if (Kernels.empty())
    return createStringError(inconvertibleErrorCode(),
                             "no @triton.jit kernel found in '%s'",
                             Ast.sourcePath().str().c_str());
  return Kernels;
}

llvm::Expected<Module> Frontend::lowerKernels(const KernelBindings &Bindings) {
  const Object &Root = Ast.root();
  const Array *Body = Root.getArray("body");
  if (!Body)
    return createStringError(inconvertibleErrorCode(),
                             "'%s' has no module body",
                             Ast.sourcePath().str().c_str());

  Module M;
  unsigned NumKernels = 0;
  for (const json::Value &Item : *Body) {
    const Object *Fn = Item.getAsObject();
    if (!Fn || typeOf(*Fn) != "FunctionDef")
      continue;

    bool IsKernel = false;
    if (const Array *Decorators = Fn->getArray("decorator_list")) {
      for (const json::Value &Decorator : *Decorators) {
        if (isJitDecorator(Decorator)) {
          IsKernel = true;
          break;
        }
      }
    }
    if (!IsKernel)
      continue;

    Function &F = M.createFunction(stringField(*Fn, "name"));
    Lowerer L(Ast, F, Bindings);
    if (Error E = L.lower(*Fn))
      return std::move(E);
    ++NumKernels;
  }

  if (NumKernels == 0)
    return createStringError(inconvertibleErrorCode(),
                             "no @triton.jit kernel found in '%s'",
                             Ast.sourcePath().str().c_str());
  return std::move(M);
}

} // namespace gpumintriton
