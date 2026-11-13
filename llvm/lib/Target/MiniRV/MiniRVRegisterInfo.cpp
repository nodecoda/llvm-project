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
#include "MiniRVInstrInfo.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"

using namespace llvm;

#define GET_REGINFO_TARGET_DESC
#include "MiniRVGenRegisterInfo.inc"

MiniRVRegisterInfo::MiniRVRegisterInfo() : MiniRVGenRegisterInfo(MiniRV::X1) {}

const MCPhysReg *
MiniRVRegisterInfo::getCalleeSavedRegs(const MachineFunction *MF) const {
  return CSR_MiniRV_SaveList;
}

const uint32_t *
MiniRVRegisterInfo::getCallPreservedMask(const MachineFunction &MF,
                                         CallingConv::ID CC) const {
  return CSR_MiniRV_RegMask;
}

BitVector MiniRVRegisterInfo::getReservedRegs(const MachineFunction &MF) const {
  BitVector Reserved(getNumRegs());
  Reserved.set(MiniRV::X0); // zero
  Reserved.set(MiniRV::X2); // stack pointer
  // X5 is the scratch register used to materialise a stack address whose
  // offset does not fit in a 12-bit immediate.  Reserving it keeps the
  // register allocator from putting a live value there.
  Reserved.set(MiniRV::X5);
  // X6 is the scratch used when a stack adjustment is both source and
  // destination, as it is in the prologue and epilogue.
  Reserved.set(MiniRV::X6);
  return Reserved;
}

bool MiniRVRegisterInfo::eliminateFrameIndex(MachineBasicBlock::iterator II,
                                             int SPAdj, unsigned FIOperandNum,
                                             RegScavenger *RS) const {
  assert(SPAdj == 0 && "Unexpected SPAdj");
  MachineInstr &MI = *II;
  MachineFunction &MF = *MI.getParent()->getParent();

  int FrameIndex = MI.getOperand(FIOperandNum).getIndex();
  int64_t Offset = static_cast<int64_t>(
                       MF.getFrameInfo().getObjectOffset(FrameIndex)) +
                   static_cast<int64_t>(MF.getFrameInfo().getStackSize()) +
                   MI.getOperand(FIOperandNum + 1).getImm();

  // The common case: the resolved offset fits in the twelve bits of the
  // instruction's immediate field.
  if (isInt<12>(Offset)) {
    MI.getOperand(FIOperandNum).ChangeToRegister(MiniRV::X2, /*isDef=*/false);
    MI.getOperand(FIOperandNum + 1).ChangeToImmediate(Offset);
    return false;
  }

  // A large frame puts the object out of reach of a 12-bit displacement.
  // LocalStackSlotAllocation normally creates a base register for such an
  // object before register allocation; when it does not, PEI has to do it
  // here, after register allocation.
  MachineBasicBlock &MBB = *MI.getParent();
  const auto *TFI = static_cast<const MiniRVFrameLowering *>(
      MF.getSubtarget().getFrameLowering());
  DebugLoc DL = MI.getDebugLoc();

  // The instruction that materialises a base register is "addi <fi>, 0".
  // Its destination is already a physical register, so the address can be
  // computed in place and the now-redundant copy dropped.
  if (MI.getOpcode() == MiniRV::ADDI) {
    Register Dest = MI.getOperand(0).getReg();
    TFI->adjustReg(MBB, II, DL, Dest, MiniRV::X2, Offset,
                   MachineInstr::NoFlags);
    MI.eraseFromParent();
    return true;
  }

  // A load or store: address it through the reserved scratch register with a
  // zero displacement.
  TFI->adjustReg(MBB, II, DL, MiniRV::X5, MiniRV::X2, Offset,
                 MachineInstr::NoFlags);
  MI.getOperand(FIOperandNum).ChangeToRegister(MiniRV::X5, /*isDef=*/false);
  MI.getOperand(FIOperandNum + 1).ChangeToImmediate(0);
  return false;
}

bool MiniRVRegisterInfo::requiresVirtualBaseRegisters(
    const MachineFunction &MF) const {
  // Large frames need a base register to reach objects more than 2047 bytes
  // away from the stack pointer.
  return true;
}

int64_t MiniRVRegisterInfo::getFrameIndexInstrOffset(const MachineInstr *MI,
                                                     int Idx) const {
  assert(MI->getOperand(Idx).isFI() && "Not a frame index operand");
  return MI->getOperand(Idx + 1).getImm();
}

bool MiniRVRegisterInfo::isFrameOffsetLegal(const MachineInstr *MI,
                                           Register BaseReg,
                                           int64_t Offset) const {
  unsigned FIOperandNum = 0;
  while (!MI->getOperand(FIOperandNum).isFI()) {
    ++FIOperandNum;
    assert(FIOperandNum < MI->getNumOperands() &&
           "Instruction does not have a frame index operand");
  }
  return isInt<12>(Offset + getFrameIndexInstrOffset(MI, FIOperandNum));
}

bool MiniRVRegisterInfo::needsFrameBaseReg(MachineInstr *MI,
                                           int64_t Offset) const {
  const MachineFrameInfo &MFI = MI->getMF()->getFrameInfo();
  // The object sits at Offset inside the local block; the block itself starts
  // LocalFrameSize bytes below the stack pointer.
  return !isFrameOffsetLegal(MI, MiniRV::X2, Offset + MFI.getLocalFrameSize());
}

Register MiniRVRegisterInfo::materializeFrameBaseRegister(
    MachineBasicBlock *MBB, int FrameIdx, int64_t Offset) const {
  MachineBasicBlock::iterator MBBI = MBB->begin();
  DebugLoc DL = MBBI != MBB->end() ? MBBI->getDebugLoc() : DebugLoc();
  MachineFunction &MF = *MBB->getParent();
  const auto &TII = *static_cast<const MiniRVInstrInfo *>(
      MF.getSubtarget().getInstrInfo());
  Register BaseReg =
      MF.getRegInfo().createVirtualRegister(&MiniRV::GPRRegClass);
  BuildMI(*MBB, MBBI, DL, TII.get(MiniRV::ADDI), BaseReg)
      .addFrameIndex(FrameIdx)
      .addImm(Offset);
  return BaseReg;
}

void MiniRVRegisterInfo::resolveFrameIndex(MachineInstr &MI, Register BaseReg,
                                          int64_t Offset) const {
  unsigned FIOperandNum = 0;
  while (!MI.getOperand(FIOperandNum).isFI())
    ++FIOperandNum;
  Offset += getFrameIndexInstrOffset(&MI, FIOperandNum);
  MI.getOperand(FIOperandNum).ChangeToRegister(BaseReg, /*isDef=*/false);
  MI.getOperand(FIOperandNum + 1).ChangeToImmediate(Offset);
}

Register MiniRVRegisterInfo::getFrameRegister(const MachineFunction &MF) const {
  return MiniRV::X2;
}
