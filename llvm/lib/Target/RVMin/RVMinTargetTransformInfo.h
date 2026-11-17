//===-- RVMinTargetTransformInfo.h - RVMin specific TTI --------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Chapter 49: the default TTI expands every vector reduction in the IR
// ExpandReductions pass, which hides llvm.vector.reduce.add from SelectionDAG.
// RVMin implements that one reduction with vredsum.vs, so keep the intrinsic
// alive when the V subset is enabled.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_RVMIN_RVMINTARGETTRANSFORMINFO_H
#define LLVM_LIB_TARGET_RVMIN_RVMINTARGETTRANSFORMINFO_H

#include "RVMinISelLowering.h"
#include "RVMinSubtarget.h"
#include "RVMinTargetMachine.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/CodeGen/BasicTTIImpl.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IntrinsicInst.h"

namespace llvm {

class RVMinTTIImpl final : public BasicTTIImplBase<RVMinTTIImpl> {
  using BaseT = BasicTTIImplBase<RVMinTTIImpl>;
  using TTI = TargetTransformInfo;

  friend BaseT;

  const RVMinSubtarget *ST;
  const RVMinTargetLowering *TLI;

  const RVMinSubtarget *getST() const { return ST; }
  const RVMinTargetLowering *getTLI() const { return TLI; }

public:
  RVMinTTIImpl(const RVMinTargetMachine *TM, const Function &F)
      : BaseT(TM, F.getDataLayout()), ST(TM->getSubtargetImpl(F)),
        TLI(ST->getTargetLowering()) {}

  bool shouldExpandReduction(const IntrinsicInst *II) const override {
    if (ST->hasStdExtV() &&
        II->getIntrinsicID() == Intrinsic::vector_reduce_add)
      return false;
    return true;
  }
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_RVMIN_RVMINTARGETTRANSFORMINFO_H
