//===-- GPUMinFrameLowering.cpp - GPUMin frame lowering -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "GPUMinFrameLowering.h"
#include "GPUMinSubtarget.h"
#include "llvm/CodeGen/MachineFunction.h"

using namespace llvm;

// The private stack grows down and keeps 16-byte alignment, which is what the
// GM32 launch model promises for the per-lane stack pointer.
GPUMinFrameLowering::GPUMinFrameLowering(const GPUMinSubtarget &STI)
    : TargetFrameLowering(StackGrowsDown, Align(16), 0, Align(16)) {}

bool GPUMinFrameLowering::hasFPImpl(const MachineFunction &MF) const {
  return false;
}

void GPUMinFrameLowering::emitPrologue(MachineFunction &MF,
                                       MachineBasicBlock &MBB) const {}

void GPUMinFrameLowering::emitEpilogue(MachineFunction &MF,
                                       MachineBasicBlock &MBB) const {}
