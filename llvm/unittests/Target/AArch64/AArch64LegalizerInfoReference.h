//===-- AArch64LegalizerInfoReference.h -------------------------*- C++ -*-==//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Frozen migration reference for the AArch64 legalizer differential test.
// The rule chains below are the oracle and must not be edited without a
// recorded re-baseline.
//
//   source-commit: 6706952f4a5a
//   source-path:   llvm/lib/Target/AArch64/GISel/AArch64LegalizerInfo.cpp
//   source-lines:  42-1589 (the constructor body, verbatim)
//   delete-after:  not planned; retirement requires an approved replacement
//                  oracle
//
// This is a verbatim copy of the hand-written rule chains that the TableGen
// emitter is meant to replace.  It exists only so the differential test has an
// oracle; the two custom hooks the reference would need are never reached
// because the test only queries getAction().
//===----------------------------------------------------------------------===//

#ifndef LLVM_UNITTESTS_TARGET_AARCH64_AARCH64LEGALIZERINFOREFERENCE_H
#define LLVM_UNITTESTS_TARGET_AARCH64_AARCH64LEGALIZERINFOREFERENCE_H

#include "llvm/CodeGen/GlobalISel/LegalizerInfo.h"

namespace llvm {

class AArch64Subtarget;

class AArch64LegalizerInfoReference : public LegalizerInfo {
public:
  explicit AArch64LegalizerInfoReference(const AArch64Subtarget &ST);

  // Never executed: the differential test only queries getAction().  The base
  // class default is llvm_unreachable, so override with a safe no-op.
  bool legalizeCustom(LegalizerHelper &, MachineInstr &,
                      LostDebugLocObserver &) const override {
    return false;
  }
};

} // namespace llvm

#endif
