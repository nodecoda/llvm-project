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

// Rewrite `lw/sw/addi ..., FI, imm` into `lw/sw/addi ..., sp, offset`.
//
// getFrameIndexReference returns the distance from sp *after* the prologue to
// the object, because the frame pointer is not used.  The instruction's own
// immediate is folded in, which is how `frameindex + 8` (a constant-indexed
// alloca) becomes one instruction.
bool RVMinRegisterInfo::eliminateFrameIndex(
    MachineBasicBlock::iterator II, int SPAdj, unsigned FIOperandNum,
    RegScavenger *RS) const {
  assert(SPAdj == 0 && "RVMin has no dynamic stack adjustments");

  MachineInstr &MI = *II;
  MachineFunction &MF = *MI.getParent()->getParent();
  const TargetFrameLowering *TFI = getFrameLowering(MF);

  int FrameIndex = MI.getOperand(FIOperandNum).getIndex();
  Register FrameReg;
  int64_t Offset =
      TFI->getFrameIndexReference(MF, FrameIndex, FrameReg).getFixed();
  Offset += MI.getOperand(FIOperandNum + 1).getImm();

  if (!isInt<12>(Offset))
    report_fatal_error("RVMin: frame offset " + Twine(Offset) +
                       " does not fit in 12 bits (frame larger than 2 KiB is "
                       "not supported yet)");

  MI.getOperand(FIOperandNum).ChangeToRegister(FrameReg, /*isDef=*/false);
  MI.getOperand(FIOperandNum + 1).ChangeToImmediate(Offset);
  return false;
}

Register RVMinRegisterInfo::getFrameRegister(const MachineFunction &MF) const {
  return RVMin::X2;
}
