//===-- GPUMinTargetTransformInfo.h - GPUMin specific TTI ------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_GPUMIN_GPUMINTARGETTRANSFORMINFO_H
#define LLVM_LIB_TARGET_GPUMIN_GPUMINTARGETTRANSFORMINFO_H

#include "GPUMinISelLowering.h"
#include "GPUMinSubtarget.h"
#include "GPUMinTargetMachine.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/CodeGen/BasicTTIImpl.h"
#include "llvm/IR/Function.h"

namespace llvm {

class GPUMinTTIImpl final : public BasicTTIImplBase<GPUMinTTIImpl> {
  using BaseT = BasicTTIImplBase<GPUMinTTIImpl>;
  using TTI = TargetTransformInfo;

  friend BaseT;

  const GPUMinSubtarget *ST;
  const GPUMinTargetLowering *TLI;

  const GPUMinSubtarget *getST() const { return ST; }
  const GPUMinTargetLowering *getTLI() const { return TLI; }

public:
  GPUMinTTIImpl(const GPUMinTargetMachine *TM, const Function &F)
      : BaseT(TM, F.getDataLayout()), ST(TM->getSubtargetImpl(F)),
        TLI(ST->getTargetLowering()) {}
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_GPUMIN_GPUMINTARGETTRANSFORMINFO_H
