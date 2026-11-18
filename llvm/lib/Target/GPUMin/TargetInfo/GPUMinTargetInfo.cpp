//===-- GPUMinTargetInfo.cpp - GPUMin target implementation ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "TargetInfo/GPUMinTargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Compiler.h"
#include "llvm/TargetParser/Triple.h"

using namespace llvm;

Target &llvm::getTheGPUMinTarget() {
  static Target TheGPUMinTarget;
  return TheGPUMinTarget;
}

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializeGPUMinTargetInfo() {
  // GPUMin has its own Triple::gpumin arch, so `llc -march=gpumin` (lookup by
  // name) and `clang --target=gpumin` (lookup by triple) find the same
  // backend.
  RegisterTarget<Triple::gpumin> X(getTheGPUMinTarget(), "gpumin",
                                   "GPUMin (SIMT GPU teaching backend) "
                                   "[experimental]",
                                   "GPUMin");
}
