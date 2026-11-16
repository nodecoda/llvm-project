//===-- RVMinTargetInfo.cpp - RVMin target implementation -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "TargetInfo/RVMinTargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Compiler.h"
#include "llvm/TargetParser/Triple.h"

using namespace llvm;

Target &llvm::getTheRVMinTarget() {
  static Target TheRVMinTarget;
  return TheRVMinTarget;
}

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializeRVMinTargetInfo() {
  // RVMin has its own Triple::rvmin arch.  Registering it here lets both
  // `llc -march=rvmin` (lookup by name) and `clang --target=rvmin`
  // (lookup by triple) find the same backend.
  RegisterTarget<Triple::rvmin> X(getTheRVMinTarget(), "rvmin",
                                  "RVMin (RV32I minimal) [experimental]",
                                  "RVMin");
}
