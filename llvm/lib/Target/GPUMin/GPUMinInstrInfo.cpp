//===-- GPUMinInstrInfo.cpp - GPUMin instruction information --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "GPUMinInstrInfo.h"
#include "GPUMinSubtarget.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"

using namespace llvm;

#define GET_INSTRINFO_CTOR_DTOR
#include "GPUMinGenInstrInfo.inc"

void GPUMinInstrInfo::anchor() {}

GPUMinInstrInfo::GPUMinInstrInfo(const GPUMinSubtarget &STI)
    : GPUMinGenInstrInfo(STI, RI), RI() {}

// The register allocator and the calling convention both materialise moves;
// GM32 spells them `mov rd, rs1`.
void GPUMinInstrInfo::copyPhysReg(MachineBasicBlock &MBB,
                                  MachineBasicBlock::iterator I,
                                  const DebugLoc &DL, Register DestReg,
                                  Register SrcReg, bool KillSrc,
                                  bool RenamableDest,
                                  bool RenamableSrc) const {
  if (!GPUMin::GPRRegClass.contains(DestReg, SrcReg))
    llvm_unreachable("GPUMin: only GPR-to-GPR copies are supported");

  BuildMI(MBB, I, DL, get(GPUMin::MOV), DestReg)
      .addReg(SrcReg, getKillRegState(KillSrc));
}

// The private stack lives in address space 3, so spills use the local-space
// forms.  The frame index stays an operand until eliminateFrameIndex turns it
// into an sp-relative displacement; that is why the base is fixed to r30 here.
void GPUMinInstrInfo::storeRegToStackSlot(MachineBasicBlock &MBB,
                                          MachineBasicBlock::iterator I,
                                          Register SrcReg, bool IsKill,
                                          int FrameIndex,
                                          const TargetRegisterClass *RC,
                                          Register VReg,
                                          MachineInstr::MIFlag Flags) const {
  BuildMI(MBB, I, MBB.findDebugLoc(I), get(GPUMin::ST_LOCAL_B32))
      .addReg(SrcReg, getKillRegState(IsKill))
      .addReg(GPUMin::R30)
      .addFrameIndex(FrameIndex);
}

void GPUMinInstrInfo::loadRegFromStackSlot(MachineBasicBlock &MBB,
                                           MachineBasicBlock::iterator I,
                                           Register DestReg, int FrameIndex,
                                           const TargetRegisterClass *RC,
                                           Register VReg, unsigned SubReg,
                                           MachineInstr::MIFlag Flags) const {
  BuildMI(MBB, I, MBB.findDebugLoc(I), get(GPUMin::LD_LOCAL_B32), DestReg)
      .addReg(GPUMin::R30)
      .addFrameIndex(FrameIndex);
}

// Branch analysis is what lets BranchFolding, tail merging and the block
// placement passes reason about GM32 control flow.  GM32 has exactly two
// terminators: BRA (unconditional) and BRA_P (conditional).  A lone
// conditional branch has no explicit "false" target; FBB stays null and
// getFallThrough() infers the layout fallthrough edge.  Never call
// MBB.getFallThrough() from here: it calls analyzeBranch() right back.
bool GPUMinInstrInfo::analyzeBranch(MachineBasicBlock &MBB,
                                    MachineBasicBlock *&TBB,
                                    MachineBasicBlock *&FBB,
                                    SmallVectorImpl<MachineOperand> &Cond,
                                    bool AllowModify) const {
  TBB = FBB = nullptr;
  Cond.clear();

  MachineBasicBlock::iterator I = MBB.end();
  if (I == MBB.begin())
    return true;

  --I;
  while (I != MBB.begin() && I->isDebugInstr())
    --I;

  if (I->getOpcode() == GPUMin::BRA) {
    TBB = I->getOperand(0).getMBB();
    if (I == MBB.begin())
      return false;
    --I;
    if (I->getOpcode() == GPUMin::BRA_P) {
      Cond.push_back(MachineOperand::CreateReg(I->getOperand(0).getReg(),
                                               /*isDef=*/false));
      FBB = I->getOperand(1).getMBB();
    }
    return false;
  }

  if (I->getOpcode() == GPUMin::BRA_P) {
    Cond.push_back(MachineOperand::CreateReg(I->getOperand(0).getReg(),
                                             /*isDef=*/false));
    TBB = I->getOperand(1).getMBB();
    FBB = nullptr;
    return false;
  }

  // No branch instruction: this is a plain fallthrough block.  Reporting it as
  // "analyzable, no branch" is what block placement expects; returning true
  // here makes it look like an un-analyzable fallthrough and asserts.
  if (I->isTerminator())
    return true;
  return false;
}

unsigned GPUMinInstrInfo::removeBranch(MachineBasicBlock &MBB,
                                       int *BytesRemoved) const {
  unsigned Count = 0;
  int Removed = 0;
  MachineBasicBlock::iterator I = MBB.end();
  while (I != MBB.begin()) {
    --I;
    if (I->isDebugInstr())
      continue;
    if (I->getOpcode() != GPUMin::BRA && I->getOpcode() != GPUMin::BRA_P) {
      ++I;
      break;
    }
    Removed += getInstSizeInBytes(*I);
    I->eraseFromParent();
    I = MBB.end();
    ++Count;
  }
  if (BytesRemoved)
    *BytesRemoved = Removed;
  return Count;
}

unsigned GPUMinInstrInfo::insertBranch(MachineBasicBlock &MBB,
                                       MachineBasicBlock *TBB,
                                       MachineBasicBlock *FBB,
                                       ArrayRef<MachineOperand> Cond,
                                       const DebugLoc &DL,
                                       int *BytesAdded) const {
  int Added = 0;
  if (Cond.empty()) {
    if (FBB) {
      BuildMI(MBB, MBB.end(), DL, get(GPUMin::BRA)).addMBB(TBB);
      Added += getInstSizeInBytes(*std::prev(MBB.end()));
    }
    if (BytesAdded)
      *BytesAdded = Added;
    return Added ? 1 : 0;
  }

  assert(Cond.size() == 1 && "GPUMin branches have one predicate operand");
  BuildMI(MBB, MBB.end(), DL, get(GPUMin::BRA_P))
      .addReg(Cond[0].getReg())
      .addMBB(TBB);
  Added += getInstSizeInBytes(*std::prev(MBB.end()));

  if (FBB) {
    BuildMI(MBB, MBB.end(), DL, get(GPUMin::BRA)).addMBB(FBB);
    Added += getInstSizeInBytes(*std::prev(MBB.end()));
  }

  if (BytesAdded)
    *BytesAdded = Added;
  return Added / 4;
}
