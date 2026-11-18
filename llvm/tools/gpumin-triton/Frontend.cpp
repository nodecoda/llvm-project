//===-- Frontend.cpp - Triton Python subset frontend ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Frontend.h"

#include "llvm/Support/Error.h"
#include <algorithm>
#include <map>
#include <vector>

using namespace llvm;
using namespace llvm::json;

namespace gpumintriton {

namespace {

// The 4x8 thread layout is the same contract LLVMEmit uses: warpid selects a
// row group and laneid a column (chapters 24 and 29).
constexpr unsigned WarpsPerBlock = 4;
constexpr unsigned LanesPerWarp = 8;
constexpr unsigned MaxElementsPerThread = 8;

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

bool isNoneConstant(const json::Value &V) {
  const Object *Node = V.getAsObject();
  if (!Node || typeOf(*Node) != "Constant")
    return false;
  const json::Value *Value = Node->get("value");
  return Value && Value->getAsNull();
}

bool isFullSlice(const json::Value &V) {
  const Object *Node = V.getAsObject();
  if (!Node || typeOf(*Node) != "Slice")
    return false;
  for (const char *Field : {"lower", "upper", "step"}) {
    const json::Value *Part = Node->get(Field);
    if (Part && !Part->getAsNull())
      return false;
  }
  return true;
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
  Expected<Value *> lowerSubscript(const Object &Node);
  Expected<Type> broadcastType(const Object &Node, Type A, Type B);
  Expected<Value *> broadcastTo(const Object &Node, Value *V, Type Dst);
  Expected<Value *> normalizeMask(const Object &Node, Value *Mask, Type PtrTy,
                                  StringRef What);
  Expected<Value *> lowerFloorDiv(const Object &Node, Value *LHS, Value *RHS,
                                  bool IsMod);
  Expected<Type> normalizePair(const Object &Node, Value *&LHS, Value *&RHS);
  Expected<std::vector<int64_t>> evalShape(const json::Value &Node);
  Expected<Type> evalDType(const json::Value &Node);
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
  unsigned BranchDepth = 0;
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
    if (BranchDepth > 0)
      return errAt(Node,
                   "return inside if/else is not supported in chapter 33");
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

  if (Type == "For") {
    const Object *Target = Node.getObject("target");
    const json::Value *IterNode = Node.get("iter");
    const Array *Body = Node.getArray("body");
    const Array *OrElse = Node.getArray("orelse");
    if (!Target || typeOf(*Target) != "Name")
      return errAt(Node, "only simple loop variables are supported");
    if (!IterNode || !Body)
      return errAt(Node, "malformed For");
    if (OrElse && !OrElse->empty())
      return errAt(Node, "for/else is not supported");

    const Object *Call = IterNode->getAsObject();
    const Object *Func = Call ? Call->getObject("func") : nullptr;
    bool IsRange = false;
    if (Func) {
      if (typeOf(*Func) == "Name" && stringField(*Func, "id") == "range") {
        IsRange = true;
      } else if (typeOf(*Func) == "Attribute" &&
                 stringField(*Func, "attr") == "static_range") {
        const Object *Recv = Func->getObject("value");
        IsRange = Recv && typeOf(*Recv) == "Name" &&
                  stringField(*Recv, "id") == "tl";
      }
    }
    if (!IsRange)
      return errAt(Node, "only tl.static_range(...) and range(...) loops "
                         "are supported in chapter 27");

    const Array *Args = Call->getArray("args");
    if (!Args || Args->empty() || Args->size() > 3)
      return errAt(*Call, "range expects one to three arguments");
    int64_t Start = 0;
    int64_t End = 0;
    int64_t Step = 1;
    if (Args->size() == 1) {
      Expected<int64_t> EndOrErr = evalInt((*Args)[0]);
      if (!EndOrErr)
        return EndOrErr.takeError();
      End = *EndOrErr;
    } else {
      Expected<int64_t> StartOrErr = evalInt((*Args)[0]);
      if (!StartOrErr)
        return StartOrErr.takeError();
      Expected<int64_t> EndOrErr = evalInt((*Args)[1]);
      if (!EndOrErr)
        return EndOrErr.takeError();
      Start = *StartOrErr;
      End = *EndOrErr;
      if (Args->size() == 3) {
        Expected<int64_t> StepOrErr = evalInt((*Args)[2]);
        if (!StepOrErr)
          return StepOrErr.takeError();
        Step = *StepOrErr;
      }
    }
    if (Step == 0)
      return errAt(*Call, "range step must not be zero");

    int64_t Count = 0;
    for (int64_t I = Start; Step > 0 ? I < End : I > End; I += Step)
      ++Count;
    if (Count > 1024)
      return errAt(Node, "static range is limited to 1024 iterations");

    // Chapter 27 unrolls compile-time loops.  The loop variable is a fresh
    // constant in every iteration, so the SSA environment simply rebinds.
    // Chapter 34 also records it in ConstValues: shared tile indices must be
    // compile-time integers, and `k` from tl.static_range is exactly that.
    std::string TargetName = stringField(*Target, "id").str();
    for (int64_t I = Start; Step > 0 ? I < End : I > End; I += Step) {
      bindName(*Target, F.createConstant(I, gpumintriton::Type::i32()));
      ConstValues[TargetName] = I;
      for (const json::Value &Stmt : *Body) {
        const Object *StmtObj = Stmt.getAsObject();
        if (!StmtObj)
          return errAt(Node, "malformed statement in loop body");
        if (Error E = lowerStmt(*StmtObj))
          return E;
      }
    }
    return Error::success();
  }

  if (Type == "If") {
    const json::Value *TestNode = Node.get("test");
    const Array *Body = Node.getArray("body");
    const Array *OrElse = Node.getArray("orelse");
    if (!TestNode || !Body)
      return errAt(Node, "malformed If");

    Expected<Value *> CondOrErr = lowerExpr(*TestNode);
    if (!CondOrErr)
      return CondOrErr.takeError();
    Value *Cond = *CondOrErr;
    if (!Cond || Cond->getType().isTensor() || !Cond->getType().isI1())
      return errAt(Node, "the if condition must be a scalar i1 in chapter 33");

    const std::map<std::string, Value *> SavedEnv = Env;
    const std::map<std::string, int64_t> SavedConst = ConstValues;

    auto LowerBody = [&](const Array &Stmts, std::vector<Op> &Out) -> Error {
      size_t Mark = F.getOpCount();
      ++BranchDepth;
      for (const json::Value &Stmt : Stmts) {
        const Object *StmtObj = Stmt.getAsObject();
        if (!StmtObj) {
          --BranchDepth;
          return errAt(Node, "malformed statement in if body");
        }
        if (Error E = lowerStmt(*StmtObj)) {
          --BranchDepth;
          return E;
        }
      }
      --BranchDepth;
      Out = F.takeOpsFrom(Mark);
      return Error::success();
    };

    // Chapter 33 keeps the branch bodies structured: a name that existed
    // before the if cannot be reassigned (that would need an scf.if result
    // and a PHI), and names created inside a branch do not escape.
    auto RejectOuterAssign = [&](const Object &At) -> Error {
      for (const auto &KV : Env) {
        auto It = SavedEnv.find(KV.first);
        if (It != SavedEnv.end() && It->second != KV.second)
          return errAt(At, Twine("assignment to '") + KV.first +
                               "' inside if is not supported in chapter 33");
      }
      return Error::success();
    };

    std::vector<Op> ThenOps;
    if (Error E = LowerBody(*Body, ThenOps))
      return E;
    if (Error E = RejectOuterAssign(Node))
      return E;

    Env = SavedEnv;
    ConstValues = SavedConst;

    std::vector<Op> ElseOps;
    if (OrElse && !OrElse->empty()) {
      if (Error E = LowerBody(*OrElse, ElseOps))
        return E;
      if (Error E = RejectOuterAssign(Node))
        return E;
    }

    Env = SavedEnv;
    ConstValues = SavedConst;
    F.createScfIf(Cond, std::move(ThenOps), std::move(ElseOps));
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
    if (OpType == "FloorDiv" || OpType == "Mod") {
      Expected<Value *> LOrErr = lowerExpr(*LHSNode);
      if (!LOrErr)
        return LOrErr.takeError();
      Expected<Value *> ROrErr = lowerExpr(*RHSNode);
      if (!ROrErr)
        return ROrErr.takeError();
      return lowerFloorDiv(Node, *LOrErr, *ROrErr, OpType == "Mod");
    }
    if (OpType == "Add")
      Kind = OpKind::AddI;
    else if (OpType == "Sub")
      Kind = OpKind::SubI;
    else if (OpType == "Mult")
      Kind = OpKind::MulI;
    else if (OpType == "Div")
      Kind = OpKind::DivI;
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

  if (Type == "Attribute") {
    StringRef Attr = stringField(Node, "attr");
    if (Attr != "T")
      return errAt(Node, ("unsupported attribute '" + Attr + "'").str());
    const json::Value *ValueNode = Node.get("value");
    if (!ValueNode)
      return errAt(Node, "malformed attribute");
    Expected<Value *> VOrErr = lowerExpr(*ValueNode);
    if (!VOrErr)
      return VOrErr.takeError();
    Value *V = *VOrErr;
    if (!V->getType().isTensor() || V->getType().getShape().size() != 2)
      return errAt(Node, "transpose requires a 2D tensor");
    return F.createTrans(V);
  }

  if (Type == "Subscript")
    return lowerSubscript(Node);

  if (Type == "Call")
    return lowerCall(Node);

  return errAt(Node, ("unsupported expression '" + Type + "'").str());
}

Expected<Value *> Lowerer::lowerBinary(const Object &Node, OpKind Kind,
                                       Value *LHS, Value *RHS) {
  if (LHS->isShared() || RHS->isShared())
    return errAt(Node, "a shared tile handle cannot be used in arithmetic; "
                       "read it with tl.load_shared_col/row");
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
      if (PtrTy.getShape() != OffsetTy.getShape()) {
        llvm::ArrayRef<int64_t> SP = PtrTy.getShape();
        llvm::ArrayRef<int64_t> SO = OffsetTy.getShape();
        if (SP.size() != 2 || SO.size() != 2)
          return errAt(Node, "pointer/offset tensor shapes differ");
        int64_t Rows = std::max(SP[0], SO[0]);
        int64_t Cols = std::max(SP[1], SO[1]);
        auto Fits = [](int64_t Dim, int64_t Want) {
          return Dim == Want || Dim == 1;
        };
        if (!Fits(SP[0], Rows) || !Fits(SP[1], Cols) || !Fits(SO[0], Rows) ||
            !Fits(SO[1], Cols))
          return errAt(Node, "pointer/offset tensor shapes differ");
        Type PtrDst = Type::tensor({Rows, Cols}, PtrTy.getElementType());
        Type OffDst = Type::tensor({Rows, Cols}, Type::i32());
        Expected<Value *> P = broadcastTo(Node, Ptr, PtrDst);
        if (!P)
          return P.takeError();
        Expected<Value *> O = broadcastTo(Node, Offset, OffDst);
        if (!O)
          return O.takeError();
        Ptr = *P;
        Offset = *O;
      }
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
    case OpKind::DivI:
      FloatKind = OpKind::DivF;
      break;
    default:
      return errAt(
          Node, "floating-point operands only support '+', '-', '*' and '/'");
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
      Expected<Type> DstOrErr = broadcastType(Node, LT, RT);
      if (!DstOrErr)
        return DstOrErr.takeError();
      Expected<Value *> L = broadcastTo(Node, LHS, *DstOrErr);
      if (!L)
        return L.takeError();
      Expected<Value *> R = broadcastTo(Node, RHS, *DstOrErr);
      if (!R)
        return R.takeError();
      return F.createBinary(FloatKind, *L, *R);
    }
    return errAt(Node, "mixed float and integer operands are not supported");
  }

  // Python's '/' is true division, so an integer-only '/' would silently
  // disagree with the source language.  Float operands were handled above;
  // integer division spells '//' (chapter 28).
  if (Kind == OpKind::DivI)
    return errAt(Node, "true division '/' requires f32 operands; use '//' for "
                       "integer division");

  if (LT.isI32() && RT.isI32())
    return F.createBinary(Kind, LHS, RHS);
  if (LT.isTensor() && RT.isI32())
    return F.createBinary(Kind, LHS, splatTo(RHS, LT));
  if (LT.isI32() && RT.isTensor())
    return F.createBinary(Kind, splatTo(LHS, RT), RHS);
  if (LT.isTensor() && RT.isTensor()) {
    Expected<Type> DstOrErr = broadcastType(Node, LT, RT);
    if (!DstOrErr)
      return DstOrErr.takeError();
    Expected<Value *> L = broadcastTo(Node, LHS, *DstOrErr);
    if (!L)
      return L.takeError();
    Expected<Value *> R = broadcastTo(Node, RHS, *DstOrErr);
    if (!R)
      return R.takeError();
    return F.createBinary(Kind, *L, *R);
  }
  return errAt(Node, "unsupported operand types for this operator");
}

Expected<Value *> Lowerer::lowerCompare(const Object &Node, CmpPred Pred,
                                        Value *LHS, Value *RHS) {
  if (LHS->isShared() || RHS->isShared())
    return errAt(Node, "a shared tile handle cannot be compared");
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
      Expected<Type> DstOrErr = broadcastType(Node, LT, RT);
      if (!DstOrErr)
        return DstOrErr.takeError();
      Expected<Value *> L = broadcastTo(Node, LHS, *DstOrErr);
      if (!L)
        return L.takeError();
      Expected<Value *> R = broadcastTo(Node, RHS, *DstOrErr);
      if (!R)
        return R.takeError();
      return F.createCmp(Pred, *L, *R);
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
    Expected<Type> DstOrErr = broadcastType(Node, LT, RT);
    if (!DstOrErr)
      return DstOrErr.takeError();
    Expected<Value *> L = broadcastTo(Node, LHS, *DstOrErr);
    if (!L)
      return L.takeError();
    Expected<Value *> R = broadcastTo(Node, RHS, *DstOrErr);
    if (!R)
      return R.takeError();
    return F.createCmp(Pred, *L, *R);
  }
  return errAt(Node, "comparisons require i32 or tensor<i32> operands");
}

Expected<Value *> Lowerer::lowerSubscript(const Object &Node) {
  const json::Value *ValueNode = Node.get("value");
  const json::Value *SliceNode = Node.get("slice");
  const Object *Tuple = SliceNode ? SliceNode->getAsObject() : nullptr;
  if (!ValueNode || !Tuple || typeOf(*Tuple) != "Tuple")
    return errAt(Node, "chapter 29 only supports [:, None] and [None, :]");
  const Array *Elts = Tuple->getArray("elts");
  if (!Elts || Elts->size() != 2)
    return errAt(Node, "chapter 29 only supports [:, None] and [None, :]");
  unsigned Axis;
  if (isFullSlice((*Elts)[0]) && isNoneConstant((*Elts)[1]))
    Axis = 1; // x[:, None] -> tensor<Nx1>
  else if (isNoneConstant((*Elts)[0]) && isFullSlice((*Elts)[1]))
    Axis = 0; // x[None, :] -> tensor<1xN>
  else
    return errAt(Node, "chapter 29 only supports [:, None] and [None, :]");
  Expected<Value *> VOrErr = lowerExpr(*ValueNode);
  if (!VOrErr)
    return VOrErr.takeError();
  Value *V = *VOrErr;
  if (!V->getType().isTensor() || V->getType().getShape().size() != 1)
    return errAt(Node, "expand_dims requires a 1D tensor");
  return F.createExpandDims(V, Axis);
}

Expected<Type> Lowerer::broadcastType(const Object &Node, Type A, Type B) {
  if (A == B)
    return A;
  if (!A.isTensor() || !B.isTensor() ||
      A.getElementType() != B.getElementType())
    return errAt(Node, "tensor type mismatch");
  llvm::ArrayRef<int64_t> SA = A.getShape();
  llvm::ArrayRef<int64_t> SB = B.getShape();
  if (SA.size() != 2 || SB.size() != 2)
    return errAt(Node, "tensor type mismatch");
  int64_t Rows = std::max(SA[0], SB[0]);
  int64_t Cols = std::max(SA[1], SB[1]);
  auto Fits = [](int64_t Dim, int64_t Want) { return Dim == Want || Dim == 1; };
  if (!Fits(SA[0], Rows) || !Fits(SA[1], Cols) || !Fits(SB[0], Rows) ||
      !Fits(SB[1], Cols))
    return errAt(Node, "tensor shapes are not broadcast-compatible");
  return Type::tensor({Rows, Cols}, A.getElementType());
}

Expected<Value *> Lowerer::broadcastTo(const Object &Node, Value *V, Type Dst) {
  if (V->getType() == Dst)
    return V;
  if (!V->getType().isTensor() || !Dst.isTensor())
    return errAt(Node, "only tensors can be broadcast");
  return F.createBroadcast(V, Dst);
}

// A mask may be a scalar i1 (splatted over the tile), a tensor with the
// pointer's shape, or a 2D tensor that broadcasts up to the pointer's shape.
// The pointer shape never grows: a load returns exactly the tile it indexed.
Expected<Value *> Lowerer::normalizeMask(const Object &Node, Value *Mask,
                                         Type PtrTy, StringRef What) {
  if (PtrTy.isPtr()) {
    if (!Mask->getType().isI1())
      return errAt(Node, "a scalar " + Twine(What) + " needs a scalar i1 mask");
    return Mask;
  }
  Type MaskTy = Mask->getType();
  if (MaskTy.isI1())
    return F.createSplat(Mask, Type::tensor(PtrTy.getShape(), Type::i1()));
  if (!MaskTy.isTensor() || !MaskTy.getElementType().isI1())
    return errAt(Node, "tl." + Twine(What) + " mask must be i1 or tensor<i1>");
  Type Dst = Type::tensor(PtrTy.getShape(), Type::i1());
  if (MaskTy == Dst)
    return Mask;
  Expected<Type> DstOrErr = broadcastType(Node, MaskTy, Dst);
  if (!DstOrErr) {
    consumeError(DstOrErr.takeError());
    return errAt(Node, "tl." + Twine(What) +
                           " mask shape is not broadcast-compatible with the "
                           "pointer");
  }
  if (*DstOrErr != Dst)
    return errAt(Node, "tl." + Twine(What) +
                           " mask shape is not broadcast-compatible with the "
                           "pointer");
  Expected<Value *> BroadcastOrErr = broadcastTo(Node, Mask, Dst);
  if (!BroadcastOrErr)
    return BroadcastOrErr.takeError();
  return *BroadcastOrErr;
}

Expected<Type> Lowerer::normalizePair(const Object &Node, Value *&LHS,
                                      Value *&RHS) {
  Type LT = LHS->getType();
  Type RT = RHS->getType();
  if (LT.isTensor() && (RT.isI32() || RT.isF32())) {
    if (LT.getElementType() != RT)
      return errAt(Node, "scalar/tensor element types differ");
    RHS = splatTo(RHS, LT);
  } else if ((LT.isI32() || LT.isF32()) && RT.isTensor()) {
    if (RT.getElementType() != LT)
      return errAt(Node, "scalar/tensor element types differ");
    LHS = splatTo(LHS, RT);
  } else if (LT.isTensor() && RT.isTensor()) {
    Expected<Type> DstOrErr = broadcastType(Node, LT, RT);
    if (!DstOrErr)
      return DstOrErr.takeError();
    Expected<Value *> L = broadcastTo(Node, LHS, *DstOrErr);
    if (!L)
      return L.takeError();
    Expected<Value *> R = broadcastTo(Node, RHS, *DstOrErr);
    if (!R)
      return R.takeError();
    LHS = *L;
    RHS = *R;
  } else if (LT != RT) {
    return errAt(Node, "operand type mismatch");
  }
  Type Ty = LHS->getType();
  Type Elem = Ty.isTensor() ? Ty.getElementType() : Ty;
  if (!Elem.isI32() && !Elem.isF32())
    return errAt(Node, "operands must be i32 or f32");
  return Ty;
}

/// Python's `//` and `%` floor the quotient; C (and arith.divsi/remsi)
/// truncate.  The correction only needs comparisons and selects, so it
/// survives without a predicate boolean op:
///
///   q1 = select(r != 0, q - 1, q)
///   qf = select((a ^ b) < 0, q1, q)
///
/// and for `%`:  r1 = select(r != 0, r + b, r),
///               rf = select((a ^ b) < 0, r1, r).
Expected<Value *> Lowerer::lowerFloorDiv(const Object &Node, Value *LHS,
                                         Value *RHS, bool IsMod) {
  Expected<Type> TyOrErr = normalizePair(Node, LHS, RHS);
  if (!TyOrErr)
    return TyOrErr.takeError();
  Type Ty = *TyOrErr;
  Type Elem = Ty.isTensor() ? Ty.getElementType() : Ty;
  if (!Elem.isI32())
    return errAt(Node, "floor division requires integer operands");

  auto ConstLike = [&](int64_t Imm) -> Value * {
    Value *C = F.createConstant(Imm, Type::i32());
    return Ty.isTensor() ? F.createSplat(C, Ty) : C;
  };

  Value *Zero = ConstLike(0);
  Value *Q = F.createBinary(OpKind::DivI, LHS, RHS);
  Value *R = F.createBinary(OpKind::RemI, LHS, RHS);
  Value *NonZero = F.createCmp(CmpPred::Ne, R, Zero);
  Value *QDown = F.createBinary(OpKind::SubI, Q, ConstLike(1));
  Value *Q1 = F.createSelect(NonZero, QDown, Q);
  Value *SignXor = F.createBinary(OpKind::XOrI, LHS, RHS);
  Value *SignDiff = F.createCmp(CmpPred::Slt, SignXor, Zero);
  if (!IsMod)
    return F.createSelect(SignDiff, Q1, Q);

  Value *RUp = F.createBinary(OpKind::AddI, R, RHS);
  Value *R1 = F.createSelect(NonZero, RUp, R);
  return F.createSelect(SignDiff, R1, R);
}

Expected<std::vector<int64_t>>
Lowerer::evalShape(const json::Value &NodeValue) {
  const Object *Node = NodeValue.getAsObject();
  if (!Node || typeOf(*Node) != "Tuple")
    return createStringError(inconvertibleErrorCode(),
                             "%s: shape must be a tuple",
                             Ast.sourcePath().str().c_str());
  const Array *Elts = Node->getArray("elts");
  if (!Elts || Elts->empty() || Elts->size() > 2)
    return errAt(*Node, "only 1D and 2D shapes are supported");
  std::vector<int64_t> Dims;
  for (const json::Value &Elt : *Elts) {
    Expected<int64_t> DimOrErr = evalInt(Elt);
    if (!DimOrErr)
      return DimOrErr.takeError();
    Dims.push_back(*DimOrErr);
  }
  return Dims;
}

Expected<Type> Lowerer::evalDType(const json::Value &NodeValue) {
  const Object *Node = NodeValue.getAsObject();
  if (!Node || typeOf(*Node) != "Attribute")
    return createStringError(inconvertibleErrorCode(),
                             "%s: dtype must be tl.int32 or tl.float32",
                             Ast.sourcePath().str().c_str());
  StringRef Attr = stringField(*Node, "attr");
  if (Attr == "int32")
    return Type::i32();
  if (Attr == "float32")
    return Type::f32();
  return errAt(*Node, "only tl.int32 and tl.float32 are supported");
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
    if (*AxisOrErr != 0 && *AxisOrErr != 1)
      return errAt(Node, "chapter 29 only supports tl.program_id(0) and "
                         "tl.program_id(1)");
    return F.createGetProgramId(static_cast<unsigned>(*AxisOrErr));
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

  if (Name == "trans") {
    const json::Value *ValueNode = GetArg(0);
    if (!ValueNode)
      return errAt(Node, "tl.trans expects one tensor");
    Expected<Value *> VOrErr = lowerExpr(*ValueNode);
    if (!VOrErr)
      return VOrErr.takeError();
    Value *V = *VOrErr;
    if (!V->getType().isTensor() || V->getType().getShape().size() != 2)
      return errAt(Node, "tl.trans requires a 2D tensor");
    return F.createTrans(V);
  }

  if (Name == "alloc_shared") {
    const json::Value *ShapeNode = GetArg(0);
    const json::Value *DTypeNode = GetArg(1);
    if (!ShapeNode || !DTypeNode)
      return errAt(Node, "tl.alloc_shared expects (shape, dtype)");
    Expected<std::vector<int64_t>> ShapeOrErr = evalShape(*ShapeNode);
    if (!ShapeOrErr)
      return ShapeOrErr.takeError();
    Expected<Type> DTOrErr = evalDType(*DTypeNode);
    if (!DTOrErr)
      return DTOrErr.takeError();
    if (ShapeOrErr->size() != 2)
      return errAt(Node, "tl.alloc_shared needs a 2D shape");
    if (!DTOrErr->isF32())
      return errAt(Node, "tl.alloc_shared only supports tl.float32 in "
                         "chapter 34");
    int64_t R = (*ShapeOrErr)[0], C = (*ShapeOrErr)[1];
    if (R <= 0 || C <= 0)
      return errAt(Node, "shared tile dimensions must be positive");
    int64_t Rows = R == 1 ? 1 : R / WarpsPerBlock;
    int64_t Cols = C == 1 ? 1 : C / LanesPerWarp;
    if ((R != 1 && R % WarpsPerBlock) || (C != 1 && C % LanesPerWarp) ||
        Rows * Cols > MaxElementsPerThread)
      return errAt(Node, "shared tile layout must be 1-or-multiple of 4 rows "
                         "and 1-or-multiple of 8 columns, with at most 8 "
                         "elements per thread");
    return F.createSharedAlloc(Type::tensor({R, C}, Type::f32()));
  }

  if (Name == "store_shared") {
    const json::Value *HandleNode = GetArg(0);
    const json::Value *ValueNode = GetArg(1);
    if (!HandleNode || !ValueNode)
      return errAt(Node, "tl.store_shared expects (handle, tile)");
    Expected<Value *> HandleOrErr = lowerExpr(*HandleNode);
    if (!HandleOrErr)
      return HandleOrErr.takeError();
    Expected<Value *> SrcOrErr = lowerExpr(*ValueNode);
    if (!SrcOrErr)
      return SrcOrErr.takeError();
    Value *Handle = *HandleOrErr;
    Value *Src = *SrcOrErr;
    if (!Handle->isShared())
      return errAt(Node, "tl.store_shared expects a handle from "
                         "tl.alloc_shared");
    if (Src->isShared())
      return errAt(Node, "tl.store_shared source must be a value tile");
    if (Src->getType() != Handle->getType())
      return errAt(Node, "tl.store_shared source shape does not match the "
                         "shared tile");
    F.createSharedStore(Handle, Src);
    return nullptr;
  }

  if (Name == "load_shared_col" || Name == "load_shared_row") {
    bool IsCol = Name == "load_shared_col";
    const json::Value *HandleNode = GetArg(0);
    const json::Value *IndexNode = GetArg(1);
    if (!HandleNode || !IndexNode)
      return errAt(Node, "tl." + Name + " expects (handle, index)");
    Expected<Value *> HandleOrErr = lowerExpr(*HandleNode);
    if (!HandleOrErr)
      return HandleOrErr.takeError();
    Value *Handle = *HandleOrErr;
    if (!Handle->isShared())
      return errAt(Node, "tl." + Name +
                             " expects a handle from "
                             "tl.alloc_shared");
    Expected<int64_t> IndexOrErr = evalInt(*IndexNode);
    if (!IndexOrErr)
      return IndexOrErr.takeError();
    Type HandleTy = Handle->getType();
    llvm::ArrayRef<int64_t> Shape = HandleTy.getShape();
    int64_t Limit = IsCol ? Shape[1] : Shape[0];
    if (*IndexOrErr < 0 || *IndexOrErr >= Limit)
      return errAt(Node, "the shared " + Twine(IsCol ? "column" : "row") +
                             " index is out of range");
    return IsCol ? F.createSharedCol(Handle, *IndexOrErr)
                 : F.createSharedRow(Handle, *IndexOrErr);
  }

  if (Name == "debug_barrier") {
    if (Args && !Args->empty())
      return errAt(Node, "tl.debug_barrier takes no arguments");
    F.createBarrier();
    return nullptr;
  }

  if (Name == "exp2") {
    const json::Value *ValueNode = GetArg(0);
    if (!ValueNode)
      return errAt(Node, "tl.exp2 expects one tensor");
    Expected<Value *> VOrErr = lowerExpr(*ValueNode);
    if (!VOrErr)
      return VOrErr.takeError();
    Value *V = *VOrErr;
    if (!V->getType().isTensor() || !V->getType().getElementType().isF32())
      return errAt(Node, "tl.exp2 requires an f32 tensor");
    return F.createExp2(V);
  }

  if (Name == "load") {
    const json::Value *PtrNode = GetArg(0);
    if (!PtrNode)
      return errAt(Node, "tl.load expects a pointer");
    Expected<Value *> PtrOrErr = lowerExpr(*PtrNode);
    if (!PtrOrErr)
      return PtrOrErr.takeError();
    Value *Ptr = *PtrOrErr;
    if (Ptr->isShared())
      return errAt(Node, "a shared handle cannot be loaded with tl.load; use "
                         "tl.load_shared_col/row");
    if (!Ptr->getType().isPtr() &&
        !(Ptr->getType().isTensor() &&
          Ptr->getType().getElementType().isPtr()))
      return errAt(Node, "tl.load expects a pointer or pointer tensor");
    Type PtrTy = Ptr->getType();
    Value *Mask = nullptr;
    auto MaskIt = Keywords.find("mask");
    if (MaskIt != Keywords.end()) {
      Expected<Value *> MaskOrErr = lowerExpr(*MaskIt->second);
      if (!MaskOrErr)
        return MaskOrErr.takeError();
      Expected<Value *> NormOrErr =
          normalizeMask(Node, *MaskOrErr, PtrTy, "load");
      if (!NormOrErr)
        return NormOrErr.takeError();
      Mask = *NormOrErr;
    }

    Value *Other = nullptr;
    auto OtherIt = Keywords.find("other");
    if (OtherIt != Keywords.end()) {
      if (!Mask)
        return errAt(Node, "tl.load(other=...) needs a mask");
      Expected<Value *> OtherOrErr = lowerExpr(*OtherIt->second);
      if (!OtherOrErr)
        return OtherOrErr.takeError();
      Other = *OtherOrErr;
      Type ResultTy = PtrTy.isTensor()
                          ? Type::tensor(PtrTy.getShape(),
                                         PtrTy.getElementType().getPointee())
                          : PtrTy.getPointee();
      if (Other->getType() != ResultTy) {
        if (!ResultTy.isTensor() ||
            Other->getType() != ResultTy.getElementType())
          return errAt(Node, "tl.load other's element type does not match the "
                             "pointer element type");
        Other = F.createSplat(Other, ResultTy);
      }
    }
    return F.createLoad(Ptr, Mask, Other);
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
    if (Ptr->isShared() || Stored->isShared())
      return errAt(Node, "a shared handle cannot be used with tl.store; use "
                         "tl.store_shared");
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
      Expected<Value *> NormOrErr =
          normalizeMask(Node, *MaskOrErr, Ptr->getType(), "store");
      if (!NormOrErr)
        return NormOrErr.takeError();
      Mask = *NormOrErr;
    }
    F.createStore(Ptr, Stored, Mask);
    return nullptr;
  }

  if (Name == "where") {
    const json::Value *CondNode = GetArg(0);
    const json::Value *XNode = GetArg(1);
    const json::Value *YNode = GetArg(2);
    if (!CondNode || !XNode || !YNode)
      return errAt(Node, "tl.where expects (condition, x, y)");
    Expected<Value *> CondOrErr = lowerExpr(*CondNode);
    if (!CondOrErr)
      return CondOrErr.takeError();
    Expected<Value *> XOrErr = lowerExpr(*XNode);
    if (!XOrErr)
      return XOrErr.takeError();
    Expected<Value *> YOrErr = lowerExpr(*YNode);
    if (!YOrErr)
      return YOrErr.takeError();
    Value *Cond = *CondOrErr;
    Value *X = *XOrErr;
    Value *Y = *YOrErr;

    Type XT = X->getType();
    Type YT = Y->getType();
    if (XT.isTensor() && YT.isI32())
      Y = splatTo(Y, XT);
    else if (XT.isI32() && YT.isTensor())
      X = splatTo(X, YT);
    else if (XT.isTensor() && YT.isTensor()) {
      if (XT != YT)
        return errAt(Node, "tl.where value shapes differ");
    }
    Type VT = X->getType();
    if (X->getType() != Y->getType())
      return errAt(Node, "tl.where values have different types");
    if (!(VT.isI32() || VT.isF32() ||
          (VT.isTensor() && (VT.getElementType().isI32() ||
                             VT.getElementType().isF32()))))
      return errAt(Node, "tl.where values must be i32 or f32");

    Type CT = Cond->getType();
    if (VT.isTensor()) {
      if (CT.isI1()) {
        Cond = splatTo(Cond, Type::tensor(VT.getShape(), Type::i1()));
      } else if (!(CT.isTensor() && CT.getElementType().isI1() &&
                   CT.getShape() == VT.getShape())) {
        return errAt(Node, "tl.where condition shape does not match the values");
      }
    } else if (!CT.isI1()) {
      return errAt(Node, "a scalar tl.where needs an i1 condition");
    }
    return F.createSelect(Cond, X, Y);
  }

  if (Name == "cdiv") {
    const json::Value *ANode = GetArg(0);
    const json::Value *BNode = GetArg(1);
    if (!ANode || !BNode)
      return errAt(Node, "tl.cdiv expects (a, b)");
    Expected<Value *> AOrErr = lowerExpr(*ANode);
    if (!AOrErr)
      return AOrErr.takeError();
    Expected<Value *> BOrErr = lowerExpr(*BNode);
    if (!BOrErr)
      return BOrErr.takeError();
    Value *A = *AOrErr;
    Value *Bv = *BOrErr;
    Expected<Type> TyOrErr = normalizePair(Node, A, Bv);
    if (!TyOrErr)
      return TyOrErr.takeError();
    Type Ty = *TyOrErr;
    Type Elem = Ty.isTensor() ? Ty.getElementType() : Ty;
    if (!Elem.isI32())
      return errAt(Node, "tl.cdiv requires integer operands");
    auto ConstLike = [&](int64_t Imm) -> Value * {
      Value *C = F.createConstant(Imm, Type::i32());
      return Ty.isTensor() ? F.createSplat(C, Ty) : C;
    };
    // ceil(a / b) = -floor(-a / b)
    Value *Zero = ConstLike(0);
    Value *NegA = F.createBinary(OpKind::SubI, Zero, A);
    Expected<Value *> QOrErr = lowerFloorDiv(Node, NegA, Bv, false);
    if (!QOrErr)
      return QOrErr.takeError();
    return F.createBinary(OpKind::SubI, Zero, *QOrErr);
  }

  if (Name == "maximum" || Name == "minimum") {
    const json::Value *ANode = GetArg(0);
    const json::Value *BNode = GetArg(1);
    if (!ANode || !BNode)
      return errAt(Node, "tl." + Name + " expects (a, b)");
    Expected<Value *> AOrErr = lowerExpr(*ANode);
    if (!AOrErr)
      return AOrErr.takeError();
    Expected<Value *> BOrErr = lowerExpr(*BNode);
    if (!BOrErr)
      return BOrErr.takeError();
    Value *A = *AOrErr;
    Value *Bv = *BOrErr;
    Expected<Type> TyOrErr = normalizePair(Node, A, Bv);
    if (!TyOrErr)
      return TyOrErr.takeError();
    CmpPred Pred = Name == "maximum" ? CmpPred::Sgt : CmpPred::Slt;
    Value *Cond = F.createCmp(Pred, A, Bv);
    return F.createSelect(Cond, A, Bv);
  }

  if (Name == "full" || Name == "zeros") {
    const json::Value *ShapeNode = GetArg(0);
    const json::Value *DTypeNode = Name == "full" ? GetArg(2) : GetArg(1);
    if (!ShapeNode || !DTypeNode)
      return errAt(Node, Name == "full"
                             ? "tl.full expects (shape, value, dtype)"
                             : "tl.zeros expects (shape, dtype)");
    Expected<std::vector<int64_t>> ShapeOrErr = evalShape(*ShapeNode);
    if (!ShapeOrErr)
      return ShapeOrErr.takeError();
    Expected<Type> DTOrErr = evalDType(*DTypeNode);
    if (!DTOrErr)
      return DTOrErr.takeError();
    Type Ty = *DTOrErr;
    Type TensorTy;
    if (ShapeOrErr->size() == 1) {
      int64_t N = (*ShapeOrErr)[0];
      if (N <= 0 || N % 32 != 0)
        return errAt(Node, "tensor size must be a positive multiple of 32");
      TensorTy = Type::tensor({N}, Ty);
    } else {
      int64_t Rows = (*ShapeOrErr)[0], Cols = (*ShapeOrErr)[1];
      if (Rows <= 0 || Cols <= 0)
        return errAt(Node, "2D shapes must be positive");
      TensorTy = Type::tensor({Rows, Cols}, Ty);
    }
    Value *V;
    if (Name == "zeros") {
      V = Ty.isF32() ? F.createConstant(0.0, Ty)
                     : F.createConstant(int64_t(0), Ty);
    } else {
      const json::Value *ValueNode = GetArg(1);
      if (!ValueNode)
        return errAt(Node, "tl.full expects (shape, value, dtype)");
      Expected<Value *> VOrErr = lowerExpr(*ValueNode);
      if (!VOrErr)
        return VOrErr.takeError();
      V = *VOrErr;
      if (V->getType() != Ty)
        return errAt(Node, "tl.full value type does not match the dtype");
    }
    return F.createSplat(V, TensorTy);
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
    llvm::ArrayRef<int64_t> SrcShape = Src->getType().getShape();
    if (SrcShape.size() != 1 && SrcShape.size() != 2)
      return errAt(Node, "reductions require a 1D or 2D tensor");

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

    Type ElemTy = Src->getType().getElementType();
    if (!ElemTy.isI32() && !ElemTy.isF32())
      return errAt(Node, "reductions require i32 or f32 elements");

    if (SrcShape.size() == 1) {
      if (Axis != 0)
        return errAt(Node, "chapter 26 only reduces axis 0");
      if (Name == "max")
        return F.createReduce(ReduceKind::Max, Src, 0);
      return F.createReduce(ReduceKind::Sum, Src, 0);
    }

    if (Axis != 0 && Axis != 1)
      return errAt(Node, "2D reductions reduce axis 0 or 1");
    auto KeepIt = Keywords.find("keep_dims");
    bool KeepDims = false;
    if (KeepIt != Keywords.end()) {
      const Object *Keep = KeepIt->second->getAsObject();
      if (!Keep || typeOf(*Keep) != "Constant")
        return errAt(Node, "keep_dims must be a constant boolean");
      auto Bool = Keep->getBoolean("value");
      if (!Bool)
        return errAt(Node, "keep_dims must be a constant boolean");
      KeepDims = *Bool;
    }
    if (!KeepDims)
      return errAt(Node, "chapter 31 requires keep_dims=True for 2D "
                         "reductions");
    Value *V =
        Name == "max"
            ? F.createReduce(ReduceKind::Max, Src, static_cast<unsigned>(Axis))
            : F.createReduce(ReduceKind::Sum, Src, static_cast<unsigned>(Axis));
    return F.createExpandDims(V, static_cast<unsigned>(Axis));
  }

  if (Name == "dot") {
    const json::Value *ANode = GetArg(0);
    const json::Value *BNode = GetArg(1);
    const json::Value *AccNode = GetArg(2);
    if (!ANode || !BNode)
      return errAt(Node, "tl.dot expects (a, b) or (a, b, acc)");
    if (Args->size() > 3)
      return errAt(Node, "tl.dot takes at most three arguments");
    if (!Keywords.empty())
      return errAt(Node, "tl.dot keyword arguments are not supported in "
                         "chapter 35");
    Expected<Value *> AOrErr = lowerExpr(*ANode);
    if (!AOrErr)
      return AOrErr.takeError();
    Expected<Value *> BOrErr = lowerExpr(*BNode);
    if (!BOrErr)
      return BOrErr.takeError();
    Value *A = *AOrErr;
    Value *B = *BOrErr;
    auto CheckTile = [&](Value *V, StringRef Side) -> Error {
      Type VTy = V->getType();
      if (!VTy.isTensor() || VTy.getShape().size() != 2 ||
          !VTy.getElementType().isF32())
        return errAt(Node, "tl.dot requires 2D f32 tiles");
      llvm::ArrayRef<int64_t> S = VTy.getShape();
      int64_t Rows = S[0] == 1 ? 1 : S[0] / WarpsPerBlock;
      int64_t Cols = S[1] == 1 ? 1 : S[1] / LanesPerWarp;
      if ((S[0] != 1 && S[0] % WarpsPerBlock) ||
          (S[1] != 1 && S[1] % LanesPerWarp) ||
          Rows * Cols > MaxElementsPerThread)
        return errAt(Node, ("tl.dot " + Twine(Side) +
                            " tile must fit 4 warps x 8 lanes with at most 8 "
                            "elements per thread")
                               .str());
      return Error::success();
    };
    if (Error E = CheckTile(A, "left"))
      return E;
    if (Error E = CheckTile(B, "right"))
      return E;
    Type ATy = A->getType();
    Type BTy = B->getType();
    llvm::ArrayRef<int64_t> AS = ATy.getShape();
    llvm::ArrayRef<int64_t> BS = BTy.getShape();
    if (AS[1] != BS[0])
      return errAt(Node, "tl.dot K dimensions do not match");
    Value *Acc;
    if (AccNode) {
      Expected<Value *> AccOrErr = lowerExpr(*AccNode);
      if (!AccOrErr)
        return AccOrErr.takeError();
      Acc = *AccOrErr;
      Type ResTy = Type::tensor({AS[0], BS[1]}, Type::f32());
      if (Acc->getType() != ResTy)
        return errAt(Node, "tl.dot acc shape does not match a*b");
    } else {
      Value *Zero = F.createConstant(0.0, Type::f32());
      Acc = F.createSplat(Zero, Type::tensor({AS[0], BS[1]}, Type::f32()));
    }
    return F.createDot(A, B, Acc);
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
