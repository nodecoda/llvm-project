//===-- gpumin-triton.cpp - C++ Triton frontend for GPUMin ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Chapter 21 driver:
//   gpumin-triton --dump-ast kernel.py
//   gpumin-triton --dump-ast-json kernel.py
//   gpumin-triton --emit-ttir kernel.py
//   gpumin-triton --ast-json kernel.ast.json --dump-ast
//
// The CPython parser is the syntax boundary; this binary owns everything from
// the AST onwards.
//
//===----------------------------------------------------------------------===//

#include "Frontend.h"
#include "LLVMEmit.h"
#include "PyAst.h"
#include "TTIR.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"
#include <memory>

using namespace llvm;
using namespace gpumintriton;

static cl::opt<std::string> Input(cl::Positional, cl::desc("<kernel.py>"),
                                  cl::init(""));
static cl::opt<std::string> AstJson("ast-json",
                                    cl::desc("Read AST JSON instead of Python"),
                                    cl::value_desc("file"), cl::init(""));
static cl::opt<std::string> Python("python",
                                   cl::desc("Python interpreter to run"),
                                   cl::value_desc("path"),
                                   cl::init("python3"));
static cl::opt<bool> DumpAst("dump-ast",
                             cl::desc("Print kernels and their arguments"),
                             cl::init(false));
static cl::opt<bool> DumpAstJson("dump-ast-json",
                                 cl::desc("Print the CPython AST as JSON"),
                                 cl::init(false));
static cl::opt<bool> EmitTTIR("emit-ttir",
                              cl::desc("Print the TTIR-min module"),
                              cl::init(false));
static cl::opt<bool> EmitLLVMIR("emit-llvm",
                                cl::desc("Print GPUMin LLVM IR"),
                                cl::init(false));
static cl::list<std::string> ConstexprBindings(
    "constexpr", cl::desc("Bind a tl.constexpr argument: NAME=INT"),
    cl::value_desc("NAME=INT"));
static cl::list<std::string> ArgTypeBindings(
    "arg-type", cl::desc("Type a runtime argument: NAME=i32|ptr"),
    cl::value_desc("NAME=TYPE"));
static cl::opt<std::string> Output("o", cl::desc("Write output to <file>"),
                                   cl::value_desc("file"), cl::init("-"));

static int reportError(Error E) {
  errs() << "gpumin-triton: " << toString(std::move(E)) << "\n";
  return 1;
}

static Expected<KernelBindings> parseBindings() {
  KernelBindings B;
  for (StringRef Spec : ConstexprBindings) {
    auto [Name, ValueText] = Spec.split('=');
    if (Name.empty() || ValueText.empty())
      return createStringError(inconvertibleErrorCode(),
                               "expected --constexpr NAME=INT, got '%s'",
                               Spec.str().c_str());
    int64_t Value = 0;
    if (ValueText.getAsInteger(10, Value))
      return createStringError(inconvertibleErrorCode(),
                               "constexpr value is not an integer: '%s'",
                               ValueText.str().c_str());
    B.Constexprs[Name.str()] = Value;
  }
  for (StringRef Spec : ArgTypeBindings) {
    auto [Name, TypeText] = Spec.split('=');
    if (Name.empty() || TypeText.empty())
      return createStringError(inconvertibleErrorCode(),
                               "expected --arg-type NAME=TYPE, got '%s'",
                               Spec.str().c_str());
    gpumintriton::Type Ty;
    if (TypeText == "i32")
      Ty = gpumintriton::Type::i32();
    else if (TypeText == "ptr" || TypeText == "ptr<i32>")
      Ty = gpumintriton::Type::ptr(gpumintriton::Type::i32());
    else
      return createStringError(inconvertibleErrorCode(),
                               "unsupported argument type '%s' (use i32 or ptr)",
                               TypeText.str().c_str());
    B.ArgTypes[Name.str()] = Ty;
  }
  return B;
}

static void printKernels(ArrayRef<KernelInfo> Kernels, raw_ostream &OS) {
  for (const KernelInfo &Kernel : Kernels) {
    OS << "kernel " << Kernel.Name << " (line " << Kernel.Line << ")\n";
    for (const KernelArg &Arg : Kernel.Args) {
      OS << "  arg " << Arg.Name;
      if (Arg.IsConstexpr)
        OS << " (constexpr)";
      OS << "\n";
    }
  }
}

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);
  cl::ParseCommandLineOptions(
      argc, argv,
      "GPUMin Triton frontend (chapter 24: TTIR to GPUMin LLVM IR)\n");

  if (Input.empty() && AstJson.empty()) {
    errs() << "gpumin-triton: expected a kernel.py or --ast-json <file>\n";
    return 1;
  }

  Expected<PyAst> AstOrErr =
      AstJson.empty() ? PyAst::loadFromPython(Input, Python)
                      : PyAst::loadFromJson(AstJson);
  if (!AstOrErr)
    return reportError(AstOrErr.takeError());
  PyAst Ast = std::move(*AstOrErr);

  if (!DumpAst && !DumpAstJson && !EmitTTIR && !EmitLLVMIR)
    DumpAst = true;

  std::error_code EC;
  std::unique_ptr<raw_fd_ostream> File;
  raw_ostream *OS = &outs();
  if (Output != "-") {
    File = std::make_unique<raw_fd_ostream>(Output, EC);
    if (EC) {
      errs() << "gpumin-triton: cannot open '" << Output
             << "': " << EC.message() << "\n";
      return 1;
    }
    OS = File.get();
  }

  if (DumpAstJson) {
    json::OStream J(*OS);
    J.value(Ast.value());
    *OS << "\n";
    return 0;
  }

  Frontend F(Ast);

  if (EmitTTIR || EmitLLVMIR) {
    Expected<KernelBindings> BindingsOrErr = parseBindings();
    if (!BindingsOrErr)
      return reportError(BindingsOrErr.takeError());
    Expected<gpumintriton::Module> ModuleOrErr =
        F.lowerKernels(*BindingsOrErr);
    if (!ModuleOrErr)
      return reportError(ModuleOrErr.takeError());
    if (EmitTTIR) {
      ModuleOrErr->print(*OS);
      return 0;
    }
    LLVMContext Ctx;
    Expected<std::unique_ptr<llvm::Module>> LLVMOrErr =
        emitLLVM(*ModuleOrErr, Ctx);
    if (!LLVMOrErr)
      return reportError(LLVMOrErr.takeError());
    (*LLVMOrErr)->print(*OS, nullptr);
    return 0;
  }

  Expected<std::vector<KernelInfo>> KernelsOrErr = F.parseKernels();
  if (!KernelsOrErr)
    return reportError(KernelsOrErr.takeError());

  printKernels(*KernelsOrErr, *OS);
  return 0;
}
