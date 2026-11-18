//===-- Frontend.h - Triton Python subset frontend -------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_GPUMIN_TRITON_FRONTEND_H
#define LLVM_TOOLS_GPUMIN_TRITON_FRONTEND_H

#include "PyAst.h"
#include "TTIR.h"
#include "llvm/Support/Error.h"
#include <string>
#include <vector>

namespace gpumintriton {

struct KernelArg {
  std::string Name;
  bool IsConstexpr = false;
  unsigned Line = 0;
};

struct KernelInfo {
  std::string Name;
  unsigned Line = 0;
  std::vector<KernelArg> Args;
};

/// The frontend has two layers:
///
///   * parseKernels(): chapter 21's recognition pass.  It answers "which
///     functions are Triton kernels and what are their parameters?".
///   * lowerKernels(): chapter 22's semantic pass.  It turns the statement
///     and expression nodes of those kernels into TTIR-min.
///
/// Both consume the same CPython AST; neither links against Triton or MLIR.
class Frontend {
public:
  explicit Frontend(const PyAst &Ast) : Ast(Ast) {}

  llvm::Expected<std::vector<KernelInfo>> parseKernels();
  llvm::Expected<Module> lowerKernels();

private:
  const PyAst &Ast;
};

} // namespace gpumintriton

#endif // LLVM_TOOLS_GPUMIN_TRITON_FRONTEND_H
