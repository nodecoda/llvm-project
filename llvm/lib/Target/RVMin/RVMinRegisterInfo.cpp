//===-- RVMinRegisterInfo.cpp - RVMin register information ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "RVMinRegisterInfo.h"
#include "RVMin.h"
#include "RVMinTargetMachine.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

#define GET_REGINFO_TARGET_DESC
#include "RVMinGenRegisterInfo.inc"

RVMinRegisterInfo::RVMinRegisterInfo() : RVMinGenRegisterInfo(RVMin::X0) {}

const MCPhysReg *
RVMinRegisterInfo::getCalleeSavedRegs(const MachineFunction *MF) const {
  static const MCPhysReg CalleeSavedRegs[] = {
      RVMin::X8,  RVMin::X9,  RVMin::X18, RVMin::X19, RVMin::X20, RVMin::X21,
      RVMin::X22, RVMin::X23, RVMin::X24, RVMin::X25, RVMin::X26, RVMin::X27,
      0};
  return CalleeSavedRegs;
}

BitVector RVMinRegisterInfo::getReservedRegs(const MachineFunction &MF) const {
  BitVector Reserved(getNumRegs());
  Reserved.set(RVMin::X0);
  Reserved.set(RVMin::X2);
  Reserved.set(RVMin::X3);
  Reserved.set(RVMin::X4);
  // Until chapter 6/8 teach the backend to save and restore the return
  // address, `ra` must not be handed out as a scratch register: a leaf
  // function still needs it for `jalr zero, 0(ra)`.
  Reserved.set(RVMin::X1);
  return Reserved;
}

bool RVMinRegisterInfo::eliminateFrameIndex(
    MachineBasicBlock::iterator II, int SPAdj, unsigned FIOperandNum,
    RegScavenger *RS) const {
  llvm_unreachable("RVMin frame index elimination is not implemented yet");
}

Register RVMinRegisterInfo::getFrameRegister(const MachineFunction &MF) const {
  return RVMin::X2;
}
