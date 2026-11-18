//===-- TTIR.h - TTIR-min skeleton for gpumin-triton ------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_GPUMIN_TRITON_TTIR_H
#define LLVM_TOOLS_GPUMIN_TRITON_TTIR_H

#include "llvm/Support/raw_ostream.h"

namespace gpumintriton {

/// Chapter 21 keeps only the module shell.  Chapter 22 adds types, values and
/// operations; the printer stays here so the textual boundary is one file.
class Module {
public:
  void print(llvm::raw_ostream &OS) const;
};

} // namespace gpumintriton

#endif // LLVM_TOOLS_GPUMIN_TRITON_TTIR_H
