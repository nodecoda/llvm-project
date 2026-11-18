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
static cl::opt<std::string> Output("o", cl::desc("Write output to <file>"),
                                   cl::value_desc("file"), cl::init("-"));

static int reportError(Error E) {
  errs() << "gpumin-triton: " << toString(std::move(E)) << "\n";
  return 1;
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
      "GPUMin Triton frontend (chapter 21: AST boundary and kernel discovery)\n");

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

  if (!DumpAst && !DumpAstJson && !EmitTTIR)
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

  if (EmitTTIR) {
    Module M;
    M.print(*OS);
    return 0;
  }

  Frontend F(Ast);
  Expected<std::vector<KernelInfo>> KernelsOrErr = F.parseKernels();
  if (!KernelsOrErr)
    return reportError(KernelsOrErr.takeError());

  printKernels(*KernelsOrErr, *OS);
  return 0;
}
