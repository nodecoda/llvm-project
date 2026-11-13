//===-- MiniRVTargetInfo.cpp - MiniRV target implementation ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "TargetInfo/MiniRVTargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Compiler.h"

using namespace llvm;

Target &llvm::getTheMiniRVTarget() {
  static Target TheMiniRVTarget;
  return TheMiniRVTarget;
}

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializeMiniRVTargetInfo() {
  RegisterTarget<Triple::minirv64> X(getTheMiniRVTarget(), "minirv64",
                                     "MiniRV (auto-td demo)", "MiniRV");
}
