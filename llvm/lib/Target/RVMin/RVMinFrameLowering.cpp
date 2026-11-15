//===-- RVMinFrameLowering.cpp - RVMin frame lowering --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "RVMinFrameLowering.h"
#include "RVMinSubtarget.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

RVMinFrameLowering::RVMinFrameLowering(const RVMinSubtarget &STI)
    : TargetFrameLowering(StackGrowsDown, Align(16), -4, Align(16)) {}

bool RVMinFrameLowering::hasFPImpl(const MachineFunction &MF) const {
  return false;
}

void RVMinFrameLowering::emitPrologue(MachineFunction &MF,
                                      MachineBasicBlock &MBB) const {
  llvm_unreachable("RVMin prologue emission is not implemented yet");
}

void RVMinFrameLowering::emitEpilogue(MachineFunction &MF,
                                      MachineBasicBlock &MBB) const {
  llvm_unreachable("RVMin epilogue emission is not implemented yet");
}
