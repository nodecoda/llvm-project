//===-- X86LegalizerInfoReference.h -----------------------------*- C++ -*-==//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Frozen migration reference for the GlobalISel legalizer differential test.
// Do not edit.
//
//   source-commit: ebac17721d12
//   source-path:   llvm/lib/Target/X86/GISel/X86LegalizerInfo.cpp
//   source-blob:   5cefdb37ddf34444f25e8a329bb5f8f51f012675f13c484e479654b98601428c
//   delete-after:  the TableGen migration PR (PR-C)
//
// This is a verbatim copy of the hand-written rule chains that the TableGen
// emitter replaced. It exists only so the differential test has an oracle.
//===----------------------------------------------------------------------===//

#ifndef LLVM_UNITTESTS_TARGET_X86_X86LEGALIZERINFOREFERENCE_H
#define LLVM_UNITTESTS_TARGET_X86_X86LEGALIZERINFOREFERENCE_H

#include "llvm/CodeGen/GlobalISel/LegalizerInfo.h"

namespace llvm {

class X86Subtarget;
class X86TargetMachine;

class X86LegalizerInfoReference : public LegalizerInfo {
  const X86Subtarget &Subtarget;

public:
  X86LegalizerInfoReference(const X86Subtarget &STI,
                            const X86TargetMachine &TM);

  // Never executed: the differential test only queries getAction(). The base
  // class default is llvm_unreachable, so override with a safe no-op.
  bool legalizeCustom(LegalizerHelper &, MachineInstr &,
                      LostDebugLocObserver &) const override {
    return false;
  }
};

} // namespace llvm

#endif
