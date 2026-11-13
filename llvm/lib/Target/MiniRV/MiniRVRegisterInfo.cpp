//===-- MiniRVRegisterInfo.cpp - MiniRV Register Information --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "MiniRVRegisterInfo.h"
#include "MiniRVFrameLowering.h"
#include "MiniRVSubtarget.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"

using namespace llvm;

#define GET_REGINFO_TARGET_DESC
#include "MiniRVGenRegisterInfo.inc"

MiniRVRegisterInfo::MiniRVRegisterInfo() : MiniRVGenRegisterInfo(MiniRV::X1) {}

const MCPhysReg *
MiniRVRegisterInfo::getCalleeSavedRegs(const MachineFunction *MF) const {
  // Everything except x0/x1/x2 is caller-saved.
  static const MCPhysReg CalleeSavedRegs[] = {0};
  return CalleeSavedRegs;
}

BitVector MiniRVRegisterInfo::getReservedRegs(const MachineFunction &MF) const {
  BitVector Reserved(getNumRegs());
  Reserved.set(MiniRV::X0); // zero
  Reserved.set(MiniRV::X1); // return address; the corpus has no call
  Reserved.set(MiniRV::X2); // stack pointer
  return Reserved;
}

bool MiniRVRegisterInfo::eliminateFrameIndex(MachineBasicBlock::iterator II,
                                             int SPAdj, unsigned FIOperandNum,
                                             RegScavenger *RS) const {
  assert(SPAdj == 0 && "Unexpected SPAdj");
  MachineInstr &MI = *II;
  MachineFunction &MF = *MI.getParent()->getParent();

  int FrameIndex = MI.getOperand(FIOperandNum).getIndex();
  int Offset = MF.getFrameInfo().getObjectOffset(FrameIndex) +
               MF.getFrameInfo().getStackSize() +
               MI.getOperand(FIOperandNum + 1).getImm();

  MI.getOperand(FIOperandNum).ChangeToRegister(MiniRV::X2, /*isDef=*/false);
  MI.getOperand(FIOperandNum + 1).ChangeToImmediate(Offset);
  return false;
}

Register MiniRVRegisterInfo::getFrameRegister(const MachineFunction &MF) const {
  return MiniRV::X2;
}
