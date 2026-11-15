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

// Generated from CSR_RVMin in RVMinCallingConv.td: ra, s0, s1, s2-s11.
const MCPhysReg *
RVMinRegisterInfo::getCalleeSavedRegs(const MachineFunction *MF) const {
  return CSR_RVMin_SaveList;
}

// The mask attached to every call instruction.  Registers in the mask are the
// ones a callee promises to preserve; everything else (a0-a7, t0-t6) is
// clobbered by the call and must be spilled by the register allocator if it is
// live across the call.
const uint32_t *RVMinRegisterInfo::getCallPreservedMask(const MachineFunction &MF,
                                                        CallingConv::ID CC) const {
  return CSR_RVMin_RegMask;
}

BitVector RVMinRegisterInfo::getReservedRegs(const MachineFunction &MF) const {
  BitVector Reserved(getNumRegs());
  Reserved.set(RVMin::X0);
  Reserved.set(RVMin::X2);
  Reserved.set(RVMin::X3);
  Reserved.set(RVMin::X4);
  // Chapter 8 stops reserving `ra`.  A leaf function still keeps its incoming
  // `ra` live across the whole body (the return uses it), so the register
  // allocator will not hand it out; a non-leaf function saves it in the
  // prologue because the call instruction defines it.
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
