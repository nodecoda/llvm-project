//===-- TTIR.cpp - TTIR-min skeleton for gpumin-triton --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "TTIR.h"

using namespace llvm;

namespace gpumintriton {

void Module::print(raw_ostream &OS) const {
  OS << "// gpumin-triton: TTIR lowering lands in chapter 22.\n";
  OS << "module {\n";
  OS << "}\n";
}

} // namespace gpumintriton
