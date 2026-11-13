//===-- MiniRVFrameLowering.cpp - MiniRV Frame Information ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The only stack adjustment instruction in the corpus is addi, so a frame is
// limited to a 12-bit signed immediate.
//
//===----------------------------------------------------------------------===//

#include "MiniRVFrameLowering.h"
#include "MiniRVInstrInfo.h"
#include "MiniRVSubtarget.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

MiniRVFrameLowering::MiniRVFrameLowering(const MiniRVSubtarget &STI)
    : TargetFrameLowering(TargetFrameLowering::StackGrowsDown, Align(8), -8,
                          Align(8)),
      STI(STI), TII(*STI.getInstrInfo()), TRI(*STI.getRegisterInfo()) {}

bool MiniRVFrameLowering::hasFPImpl(const MachineFunction &MF) const {
  // The stack pointer never moves after the prologue -- there are no calls and
  // no dynamic allocas -- so frame indices can stay sp-relative and a frame
  // pointer would only cost a register.
  return false;
}

void MiniRVFrameLowering::emitPrologue(MachineFunction &MF,
                                       MachineBasicBlock &MBB) const {
  uint64_t StackSize = MF.getFrameInfo().getStackSize();
  if (!StackSize)
    return;

  if (!isInt<12>(StackSize))
    report_fatal_error("MiniRV frames must fit in a 12-bit immediate");
  DebugLoc DL =
      MBB.begin() != MBB.end() ? MBB.begin()->getDebugLoc() : DebugLoc();
  BuildMI(MBB, MBB.begin(), DL, TII.get(MiniRV::ADDI), MiniRV::X2)
      .addReg(MiniRV::X2)
      .addImm(-static_cast<int64_t>(StackSize));
}

void MiniRVFrameLowering::emitEpilogue(MachineFunction &MF,
                                       MachineBasicBlock &MBB) const {
  uint64_t StackSize = MF.getFrameInfo().getStackSize();
  if (!StackSize)
    return;

  MachineBasicBlock::iterator MBBI = MBB.getLastNonDebugInstr();
  DebugLoc DL = MBBI->getDebugLoc();
  BuildMI(MBB, MBBI, DL, TII.get(MiniRV::ADDI), MiniRV::X2)
      .addReg(MiniRV::X2)
      .addImm(static_cast<int64_t>(StackSize));
}
