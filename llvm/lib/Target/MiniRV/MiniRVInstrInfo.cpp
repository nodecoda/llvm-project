//===-- MiniRVInstrInfo.cpp - MiniRV Instruction Information --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "MiniRVInstrInfo.h"
#include "MiniRVSubtarget.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"

using namespace llvm;

#define GET_INSTRINFO_CTOR_DTOR
#include "MiniRVGenInstrInfo.inc"

/// The six comparison records, in the order the taxonomy lists them.
static bool isCondBranch(unsigned Opcode) {
  switch (Opcode) {
  case MiniRV::BEQ:
  case MiniRV::BNE:
  case MiniRV::BLT:
  case MiniRV::BGE:
  case MiniRV::BLTU:
  case MiniRV::BGEU:
    return true;
  default:
    return false;
  }
}

bool MiniRVInstrInfo::analyzeBranch(MachineBasicBlock &MBB,
                                    MachineBasicBlock *&TBB,
                                    MachineBasicBlock *&FBB,
                                    SmallVectorImpl<MachineOperand> &Cond,
                                    bool AllowModify) const {
  TBB = FBB = nullptr;
  Cond.clear();

  MachineBasicBlock::reverse_iterator I = MBB.rbegin(), E = MBB.rend();
  while (I != E && I->isDebugInstr())
    ++I;
  if (I == E)
    return false;

  // A conditional branch is the last instruction: the false edge is the
  // fallthrough, so FBB stays null.
  if (isCondBranch(I->getOpcode())) {
    Cond.push_back(MachineOperand::CreateImm(I->getOpcode()));
    Cond.push_back(I->getOperand(0));
    Cond.push_back(I->getOperand(1));
    TBB = I->getOperand(2).getMBB();
    return false;
  }

  if (I->getOpcode() != MiniRV::PseudoBR)
    return true;

  // An unconditional branch, possibly preceded by a conditional one.
  TBB = I->getOperand(0).getMBB();
  ++I;
  while (I != E && I->isDebugInstr())
    ++I;
  if (I != E && isCondBranch(I->getOpcode())) {
    Cond.push_back(MachineOperand::CreateImm(I->getOpcode()));
    Cond.push_back(I->getOperand(0));
    Cond.push_back(I->getOperand(1));
    FBB = TBB;
    TBB = I->getOperand(2).getMBB();
  }
  return false;
}

unsigned MiniRVInstrInfo::removeBranch(MachineBasicBlock &MBB,
                                       int *BytesRemoved) const {
  unsigned Count = 0;
  if (BytesRemoved)
    *BytesRemoved = 0;

  MachineBasicBlock::iterator I = MBB.end();
  while (I != MBB.begin()) {
    --I;
    if (I->isDebugInstr())
      continue;
    if (I->getOpcode() != MiniRV::PseudoBR && !isCondBranch(I->getOpcode()))
      break;
    if (BytesRemoved)
      *BytesRemoved += getInstSizeInBytes(*I);
    I->eraseFromParent();
    I = MBB.end();
    ++Count;
  }
  return Count;
}

unsigned MiniRVInstrInfo::insertBranch(
    MachineBasicBlock &MBB, MachineBasicBlock *TBB, MachineBasicBlock *FBB,
    ArrayRef<MachineOperand> Cond, const DebugLoc &DL, int *BytesAdded) const {
  assert(TBB && "insertBranch must not be told to insert a fallthrough");
  assert((Cond.size() == 3 || Cond.size() == 0) &&
         "MiniRV branch conditions have two components");
  if (BytesAdded)
    *BytesAdded = 0;

  if (Cond.empty()) {
    assert(!FBB && "unconditional branch with a false target");
    BuildMI(&MBB, DL, get(MiniRV::PseudoBR)).addMBB(TBB);
    if (BytesAdded)
      *BytesAdded += 4;
    return 1;
  }

  // The condition carries the opcode and the two compared registers.
  unsigned Opcode = Cond[0].getImm();
  BuildMI(&MBB, DL, get(Opcode)).add(Cond[1]).add(Cond[2]).addMBB(TBB);
  if (BytesAdded)
    *BytesAdded += 4;

  if (FBB) {
    BuildMI(&MBB, DL, get(MiniRV::PseudoBR)).addMBB(FBB);
    if (BytesAdded)
      *BytesAdded += 4;
    return 2;
  }
  return 1;
}

MiniRVInstrInfo::MiniRVInstrInfo(const MiniRVSubtarget &STI)
    : MiniRVGenInstrInfo(STI, RI, MiniRV::ADJCALLSTACKDOWN,
                         MiniRV::ADJCALLSTACKUP),
      RI() {}

void MiniRVInstrInfo::storeRegToStackSlot(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MI, Register SrcReg,
    bool IsKill, int FrameIndex, const TargetRegisterClass *RC, Register VReg,
    MachineInstr::MIFlag Flags) const {
  // Every register in GPR is 64 bits wide, so one record covers all spills.
  BuildMI(MBB, MI, MBB.findDebugLoc(MI), get(MiniRV::SD))
      .addReg(SrcReg, getKillRegState(IsKill))
      .addFrameIndex(FrameIndex)
      .addImm(0);
}

void MiniRVInstrInfo::loadRegFromStackSlot(MachineBasicBlock &MBB,
                                           MachineBasicBlock::iterator MI,
                                           Register DestReg, int FrameIndex,
                                           const TargetRegisterClass *RC,
                                           Register VReg, unsigned SubReg,
                                           MachineInstr::MIFlag Flags) const {
  BuildMI(MBB, MI, MBB.findDebugLoc(MI), get(MiniRV::LD), DestReg)
      .addFrameIndex(FrameIndex)
      .addImm(0);
}

void MiniRVInstrInfo::copyPhysReg(MachineBasicBlock &MBB,
                                  MachineBasicBlock::iterator MI,
                                  const DebugLoc &DL, Register DestReg,
                                  Register SrcReg, bool KillSrc,
                                  bool RenamableDest, bool RenamableSrc) const {
  // The corpus has no move instruction, but addi rd, rs, 0 is one.
  BuildMI(MBB, MI, DL, get(MiniRV::ADDI), DestReg)
      .addReg(SrcReg, getKillRegState(KillSrc))
      .addImm(0);
}
