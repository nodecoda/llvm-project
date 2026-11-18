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

/// Chapter 21 only recognises kernels and their signatures.  The TTIR
/// lowering starts in chapter 22; keeping the recognition pass separate makes
/// that boundary explicit and testable.
class Frontend {
public:
  explicit Frontend(const PyAst &Ast) : Ast(Ast) {}

  llvm::Expected<std::vector<KernelInfo>> parseKernels();

private:
  const PyAst &Ast;
};

} // namespace gpumintriton

#endif // LLVM_TOOLS_GPUMIN_TRITON_FRONTEND_H
