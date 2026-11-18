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

/// Lowers one kernel body into one TTIR function.
///
/// Chapter 22 is deliberately scalar-only: every runtime argument is i32 and
/// every expression must be an integer literal, a name, or a pure integer
/// operation.  Calls (tl.program_id, tl.load, ...) are chapter 23.
class Lowerer {
public:
  Lowerer(const PyAst &Ast, Function &F) : Ast(Ast), F(F) {}

  Error lower(const Object &Fn);

private:
  Error errAt(const Object &Node, const Twine &Msg) const;
  Error lowerStmt(const Object &Node);
  Expected<Value *> lowerExpr(const json::Value &Node);
  Expected<Value *> lowerExpr(const Object &Node);

  const PyAst &Ast;
  Function &F;
  std::map<std::string, Value *> Env;
  bool SawReturn = false;
};

Error Lowerer::errAt(const Object &Node, const Twine &Msg) const {
  return createStringError(inconvertibleErrorCode(), "%s:%u:%u: %s",
                           Ast.sourcePath().str().c_str(), lineOf(Node),
                           columnOf(Node), Msg.str().c_str());
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
    if (Targets->size() != 1) {
      return errAt(Node, "multiple assignment targets are not supported "
                         "in chapter 22");
    }
    const Object *Target = (*Targets)[0].getAsObject();
    if (!Target || typeOf(*Target) != "Name")
      return errAt(Node, "only simple name assignment is supported in "
                         "chapter 22 (tuple targets arrive in chapter 23)");

    Expected<Value *> ValueOrErr = lowerExpr(*ValueNode);
    if (!ValueOrErr)
      return ValueOrErr.takeError();
    Env[stringField(*Target, "id").str()] = *ValueOrErr;
    return Error::success();
  }

  if (Type == "Return") {
    if (const json::Value *ValueNode = Node.get("value")) {
      Expected<Value *> ValueOrErr = lowerExpr(*ValueNode);
      if (!ValueOrErr)
        return ValueOrErr.takeError();
      Value *V = *ValueOrErr;
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

  return errAt(Node, ("unsupported statement '" + Type + "' in chapter 22")
                         .str());
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
    if (auto Imm = Node.getInteger("value"))
      return F.createConstant(*Imm, Type::i32());
    return errAt(Node, "only integer constants are supported in chapter 22");
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
      return errAt(Node, ("unsupported binary operator '" + OpType + "'")
                             .str());

    Expected<Value *> LHSOrErr = lowerExpr(*LHSNode);
    if (!LHSOrErr)
      return LHSOrErr.takeError();
    Expected<Value *> RHSOrErr = lowerExpr(*RHSNode);
    if (!RHSOrErr)
      return RHSOrErr.takeError();
    Value *LHS = *LHSOrErr;
    Value *RHS = *RHSOrErr;
    if (!LHS->getType().isI32() || !RHS->getType().isI32())
      return errAt(Node, "chapter 22 only supports i32 operands");
    return F.createBinary(Kind, LHS, RHS);
  }

  if (Type == "Compare") {
    const Array *Ops = Node.getArray("ops");
    const json::Value *LHSNode = Node.get("left");
    const Array *Comparators = Node.getArray("comparators");
    if (!Ops || Ops->size() != 1 || !LHSNode || !Comparators ||
        Comparators->size() != 1)
      return errAt(Node, "chained comparisons are not supported in chapter 22");

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
    Value *LHS = *LHSOrErr;
    Value *RHS = *RHSOrErr;
    if (!LHS->getType().isI32() || !RHS->getType().isI32())
      return errAt(Node, "chapter 22 only compares i32 operands");
    return F.createCmp(Pred, LHS, RHS);
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
      if (!Operand->getType().isI32())
        return errAt(Node, "unary minus requires an i32 operand");
      Value *Zero = F.createConstant(0, Type::i32());
      return F.createBinary(OpKind::SubI, Zero, Operand);
    }
    return errAt(Node, ("unsupported unary operator '" + OpType + "'").str());
  }

  if (Type == "Call") {
    return errAt(Node, "unsupported expression 'Call' in chapter 22 "
                       "(tl.program_id/tl.load/tl.store arrive in chapter 23)");
  }

  return errAt(Node, ("unsupported expression '" + Type + "'").str());
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
    // tl.constexpr parameters are compile-time values, not runtime arguments.
    if (Arg.IsConstexpr)
      continue;
    Env[Arg.Name] = F.addArg(Arg.Name, Type::i32());
  }
  if (ArgsObj->getObject("vararg") || ArgsObj->getObject("kwarg"))
    return errAt(Fn, "*args/**kwargs are not supported in chapter 22");

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

llvm::Expected<Module> Frontend::lowerKernels() {
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
    Lowerer L(Ast, F);
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
