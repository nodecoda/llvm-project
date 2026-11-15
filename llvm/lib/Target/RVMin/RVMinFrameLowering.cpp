//===-- RVMinFrameLowering.cpp - RVMin frame lowering --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "RVMinFrameLowering.h"
#include "RVMinSubtarget.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

RVMinFrameLowering::RVMinFrameLowering(const RVMinSubtarget &STI)
    : TargetFrameLowering(StackGrowsDown, Align(16), 0, Align(16)) {}

bool RVMinFrameLowering::hasFPImpl(const MachineFunction &MF) const {
  return false;
}

void RVMinFrameLowering::emitPrologue(MachineFunction &MF,
                                      MachineBasicBlock &MBB) const {
  // Chapter 3 functions have no stack objects, so there is nothing to do.
  // Chapter 6 adds the stack adjustment, callee-saved spills and ra.
  if (MF.getFrameInfo().getStackSize() == 0)
    return;
  llvm_unreachable("RVMin stack frames are not implemented yet (chapter 6)");
}

void RVMinFrameLowering::emitEpilogue(MachineFunction &MF,
                                      MachineBasicBlock &MBB) const {
  if (MF.getFrameInfo().getStackSize() == 0)
    return;
  llvm_unreachable("RVMin stack frames are not implemented yet (chapter 6)");
}
