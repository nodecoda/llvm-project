//===-- GPUMinFrameLowering.cpp - GPUMin frame lowering -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Chapter 07: the private stack.
//
// The layout is deliberately the simplest one that is still correct:
//
//   * no frame pointer; every slot is addressed from sp (r30);
//   * the frame grows down and stays 16-byte aligned;
//   * lr is saved in the frame of any function that makes a call, because the
//     callee overwrites it;
//   * r20-r27 are callee-saved and go through the generic callee-saved spill
//     machinery (storeRegToStackSlot/loadRegFromStackSlot in GPUMinInstrInfo).
//
//===----------------------------------------------------------------------===//

#include "GPUMinFrameLowering.h"
#include "GPUMinInstrInfo.h"
#include "GPUMinSubtarget.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/RegisterScavenging.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

GPUMinFrameLowering::GPUMinFrameLowering(const GPUMinSubtarget &STI)
    : TargetFrameLowering(StackGrowsDown, Align(16), 0, Align(16)) {}

bool GPUMinFrameLowering::hasFPImpl(const MachineFunction &MF) const {
  return false;
}

void GPUMinFrameLowering::determineCalleeSaves(MachineFunction &MF,
                                               BitVector &SavedRegs,
                                               RegScavenger *RS) const {
  TargetFrameLowering::determineCalleeSaves(MF, SavedRegs, RS);

  // A call clobbers lr.  Saving it is not optional and not something the
  // register allocator can decide: it is a property of the call itself.
  if (MF.getFrameInfo().hasCalls())
    SavedRegs.set(GPUMin::R31);
}

void GPUMinFrameLowering::emitPrologue(MachineFunction &MF,
                                       MachineBasicBlock &MBB) const {
  MachineFrameInfo &MFI = MF.getFrameInfo();
  uint64_t StackSize = MFI.getStackSize();
  if (!StackSize)
    return;

  if (!isInt<16>(-static_cast<int64_t>(StackSize)))
    report_fatal_error("GPUMin: stack frame does not fit the addi immediate");

  const GPUMinInstrInfo &TII = *MF.getSubtarget<GPUMinSubtarget>().getInstrInfo();
  MachineBasicBlock::iterator MBBI = MBB.begin();
  DebugLoc DL = MBBI != MBB.end() ? MBBI->getDebugLoc() : DebugLoc();

  // Insert before the callee-saved spills PEI already placed at the top of the
  // block, so every spill offset is relative to the *new* sp.
  BuildMI(MBB, MBBI, DL, TII.get(GPUMin::ADDI), GPUMin::R30)
      .addReg(GPUMin::R30)
      .addImm(-static_cast<int64_t>(StackSize));
}

void GPUMinFrameLowering::emitEpilogue(MachineFunction &MF,
                                       MachineBasicBlock &MBB) const {
  MachineFrameInfo &MFI = MF.getFrameInfo();
  uint64_t StackSize = MFI.getStackSize();
  if (!StackSize)
    return;

  const GPUMinInstrInfo &TII = *MF.getSubtarget<GPUMinSubtarget>().getInstrInfo();
  // After the callee-saved restores: PEI inserts those before the first
  // terminator, and inserting here puts the deallocation after them.
  MachineBasicBlock::iterator MBBI = MBB.getFirstTerminator();
  DebugLoc DL = MBBI != MBB.end() ? MBBI->getDebugLoc() : DebugLoc();

  BuildMI(MBB, MBBI, DL, TII.get(GPUMin::ADDI), GPUMin::R30)
      .addReg(GPUMin::R30)
      .addImm(static_cast<int64_t>(StackSize));
}
