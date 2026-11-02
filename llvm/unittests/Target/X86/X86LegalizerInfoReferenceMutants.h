//===-- X86LegalizerInfoReferenceMutants.h ----------------------*- C++ -*-==//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Mutation fixtures built from X86LegalizerInfoReference.cpp.  CMake copies the
// frozen reference, renames the class and replaces exactly one rule; the
// copies are test-only and never linked into the target.  They exist so the
// kill matrix can corrupt the independent oracle as well as the .td table.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_UNITTESTS_TARGET_X86_X86LEGALIZERINFOREFERENCEMUTANTS_H
#define LLVM_UNITTESTS_TARGET_X86_X86LEGALIZERINFOREFERENCEMUTANTS_H

#include "llvm/CodeGen/GlobalISel/LegalizerInfo.h"

namespace llvm {

class X86Subtarget;
class X86TargetMachine;

class X86LegalizerInfoReferenceMutant1 : public LegalizerInfo {
  const X86Subtarget &Subtarget;

public:
  X86LegalizerInfoReferenceMutant1(const X86Subtarget &STI,
                                   const X86TargetMachine &TM);

  bool legalizeCustom(LegalizerHelper &, MachineInstr &,
                      LostDebugLocObserver &) const override {
    return false;
  }
};

class X86LegalizerInfoReferenceMutant2 : public LegalizerInfo {
  const X86Subtarget &Subtarget;

public:
  X86LegalizerInfoReferenceMutant2(const X86Subtarget &STI,
                                   const X86TargetMachine &TM);

  bool legalizeCustom(LegalizerHelper &, MachineInstr &,
                      LostDebugLocObserver &) const override {
    return false;
  }
};

} // namespace llvm

#endif
