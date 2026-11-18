//===-- GPUMinRegisterInfo.cpp - GPUMin register information --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "GPUMinRegisterInfo.h"
#include "GPUMinSubtarget.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/RegisterScavenging.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

#define GET_REGINFO_TARGET_DESC
#include "GPUMinGenRegisterInfo.inc"

GPUMinRegisterInfo::GPUMinRegisterInfo() : GPUMinGenRegisterInfo(GPUMin::R31) {}

// Generated from CSR_GPUMin in GPUMinCallingConv.td: s0-s7 (r20-r27).  lr is
// reserved, not in this list; the frame lowering saves it explicitly.
const MCPhysReg *
GPUMinRegisterInfo::getCalleeSavedRegs(const MachineFunction *MF) const {
  return CSR_GPUMin_SaveList;
}

const uint32_t *
GPUMinRegisterInfo::getCallPreservedMask(const MachineFunction &MF,
                                         CallingConv::ID CC) const {
  return CSR_GPUMin_RegMask;
}

BitVector GPUMinRegisterInfo::getReservedRegs(const MachineFunction &MF) const {
  BitVector Reserved(getNumRegs());
  // r0 is hardwired to zero; code generation must never allocate it as a
  // destination.  r30 is the private-stack pointer and is owned by the frame
  // lowering (chapter 07); until then it is reserved outright.
  Reserved.set(GPUMin::R0);
  Reserved.set(GPUMin::R30);
  // lr belongs to calls: CALL writes it implicitly and RET reads it, so the
  // allocator must never hand it out as a general-purpose register.
  Reserved.set(GPUMin::R31);
  return Reserved;
}

bool GPUMinRegisterInfo::eliminateFrameIndex(MachineBasicBlock::iterator II,
                                             int SPAdj, unsigned FIOperandNum,
                                             RegScavenger *RS) const {
  MachineInstr &MI = *II;
  MachineBasicBlock &MBB = *MI.getParent();
  MachineFunction &MF = *MBB.getParent();
  MachineFrameInfo &MFI = MF.getFrameInfo();
  int FI = MI.getOperand(FIOperandNum).getIndex();

  // No frame pointer: every object is addressed from sp.  Objects grow down,
  // so an object's offset is its (negative) frame offset plus the full frame
  // size; SPAdj accounts for a caller that has already moved sp.
  int64_t Offset =
      MFI.getObjectOffset(FI) + (int64_t)MFI.getStackSize() + SPAdj;

  if (isInt<11>(Offset)) {
    MI.getOperand(FIOperandNum).ChangeToImmediate(Offset);
    return false;
  }

  // The M format cannot reach this slot.  Materialise the address in a
  // scratch register instead of truncating the displacement.
  if (!RS)
    report_fatal_error("GPUMin: frame offset does not fit 11 bits and no "
                       "register scavenger is available");
  Register Scratch =
      RS->scavengeRegisterBackwards(GPUMin::GPRRegClass, II,
                                    /*RestoreAfter=*/false, SPAdj);
  if (!Scratch)
    report_fatal_error("GPUMin: no scratch register for a large frame offset");

  BuildMI(MBB, II, MI.getDebugLoc(),
          MF.getSubtarget().getInstrInfo()->get(GPUMin::ADDI), Scratch)
      .addReg(GPUMin::R30)
      .addImm(Offset);
  MI.getOperand(FIOperandNum - 1).setReg(Scratch);
  MI.getOperand(FIOperandNum).ChangeToImmediate(0);
  return true;
}

Register GPUMinRegisterInfo::getFrameRegister(const MachineFunction &MF) const {
  return GPUMin::R30;
}
