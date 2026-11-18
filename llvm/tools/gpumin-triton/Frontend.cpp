//===-- Frontend.cpp - Triton Python subset frontend ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Frontend.h"

#include "llvm/Support/Error.h"

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

bool isJitDecorator(const Value &Decorator) {
  const Object *Node = Decorator.getAsObject();
  if (!Node)
    return false;
  StringRef Type = typeOf(*Node);
  if (Type == "Name")
    return stringField(*Node, "id") == "jit";
  if (Type == "Attribute")
    return stringField(*Node, "attr") == "jit";
  if (Type == "Call") {
    if (const Value *Func = Node->get("func"))
      return isJitDecorator(*Func);
  }
  return false;
}

bool isConstexprAnnotation(const Value &Annotation) {
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
  for (const Value &Item : *Args) {
    const Object *Arg = Item.getAsObject();
    if (!Arg || typeOf(*Arg) != "arg")
      continue;
    KernelArg A;
    A.Name = stringField(*Arg, "arg").str();
    A.Line = static_cast<unsigned>(Arg->getInteger("lineno").value_or(0));
    if (const Value *Annotation = Arg->get("annotation"))
      A.IsConstexpr = isConstexprAnnotation(*Annotation);
    Out.push_back(std::move(A));
  }
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
  for (const Value &Item : *Body) {
    const Object *Fn = Item.getAsObject();
    if (!Fn || typeOf(*Fn) != "FunctionDef")
      continue;

    bool IsKernel = false;
    if (const Array *Decorators = Fn->getArray("decorator_list")) {
      for (const Value &Decorator : *Decorators) {
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

} // namespace gpumintriton
