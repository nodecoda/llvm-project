//===-- MiniRVFrameLowering.cpp - MiniRV Frame Information ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A frame that fits in a 12-bit signed immediate is adjusted with a single
// addi.  Larger frames materialise the adjustment in a scratch register with
// lui+addi and then add it, which is the same sequence the compiler uses to
// materialise any other 32-bit constant.
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
    : TargetFrameLowering(TargetFrameLowering::StackGrowsDown, Align(8), 0,
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

  if (!isInt<32>(StackSize))
    report_fatal_error("MiniRV frames must fit in a 32-bit immediate");
  DebugLoc DL =
      MBB.begin() != MBB.end() ? MBB.begin()->getDebugLoc() : DebugLoc();
  // The adjustment has to be the first thing in the function: the callee-saved
  // spills that PEI inserts afterwards use offsets relative to the new SP.
  adjustReg(MBB, MBB.begin(), DL, MiniRV::X2, MiniRV::X2,
            -static_cast<int64_t>(StackSize), MachineInstr::FrameSetup);
}

void MiniRVFrameLowering::emitEpilogue(MachineFunction &MF,
                                       MachineBasicBlock &MBB) const {
  uint64_t StackSize = MF.getFrameInfo().getStackSize();
  if (!StackSize)
    return;

  // The restores of the callee-saved registers sit between the adjustment and
  // the return, so the SP must be released after them.
  MachineBasicBlock::iterator MBBI = MBB.getLastNonDebugInstr();
  DebugLoc DL = MBBI->getDebugLoc();
  adjustReg(MBB, MBBI, DL, MiniRV::X2, MiniRV::X2,
            static_cast<int64_t>(StackSize), MachineInstr::FrameDestroy);
}

void MiniRVFrameLowering::adjustReg(MachineBasicBlock &MBB,
                                    MachineBasicBlock::iterator II,
                                    const DebugLoc &DL, Register DestReg,
                                    Register SrcReg, int64_t Val,
                                    MachineInstr::MIFlag Flag) const {
  if (DestReg == SrcReg && Val == 0)
    return;

  if (isInt<12>(Val)) {
    BuildMI(MBB, II, DL, TII.get(MiniRV::ADDI), DestReg)
        .addReg(SrcReg)
        .addImm(Val)
        .setMIFlag(Flag);
    return;
  }

  assert(isInt<32>(Val) && "MiniRV cannot materialise a larger adjustment");
  // Round the high part away from zero so that the low part always fits in
  // the signed twelve bits of addi.  When the destination is also the source
  // the materialised constant cannot be built in place -- lui would clobber
  // the value that still has to be added -- so it goes through a scratch
  // register.
  int64_t Hi20 = (Val + 0x800) >> 12;
  int64_t Lo12 = Val - (Hi20 << 12);
  Register Scratch = DestReg == SrcReg ? Register(MiniRV::X6) : DestReg;
  BuildMI(MBB, II, DL, TII.get(MiniRV::LUI), Scratch)
      .addImm(Hi20)
      .setMIFlag(Flag);
  BuildMI(MBB, II, DL, TII.get(MiniRV::ADDI), Scratch)
      .addReg(Scratch)
      .addImm(Lo12)
      .setMIFlag(Flag);
  BuildMI(MBB, II, DL, TII.get(MiniRV::ADD), DestReg)
      .addReg(Scratch)
      .addReg(SrcReg)
      .setMIFlag(Flag);
}

MachineBasicBlock::iterator MiniRVFrameLowering::eliminateCallFramePseudoInstr(
    MachineFunction &MF, MachineBasicBlock &MBB,
    MachineBasicBlock::iterator MI) const {
  // The outgoing argument area is reserved in the prologue, so the pseudo has
  // no work to do at the call site.
  return MBB.erase(MI);
}
