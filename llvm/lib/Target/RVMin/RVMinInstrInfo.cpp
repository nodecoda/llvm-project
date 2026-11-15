//===-- RVMinInstrInfo.cpp - RVMin instruction information ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "RVMinInstrInfo.h"
#include "RVMin.h"
#include "RVMinSubtarget.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include <iterator>

using namespace llvm;

#define GET_INSTRINFO_CTOR_DTOR
#include "RVMinGenInstrInfo.inc"

void RVMinInstrInfo::anchor() {}

RVMinInstrInfo::RVMinInstrInfo(const RVMinSubtarget &STI)
    : RVMinGenInstrInfo(STI, RI, RVMin::ADJCALLSTACKDOWN,
                        RVMin::ADJCALLSTACKUP),
      RI() {}

unsigned RVMinInstrInfo::getInstSizeInBytes(const MachineInstr &MI) const {
  if (MI.isDebugInstr() || MI.isCFIInstruction())
    return 0;
  // Real RV32I instructions are always 4 bytes.  Pseudos use their declared
  // Size (PseudoRET is 4 after expansion, ADJCALLSTACK* are 0).
  return MI.getDesc().getSize();
}

// RV32I has no move instruction; `addi rd, rs, 0` is the canonical copy.
void RVMinInstrInfo::copyPhysReg(MachineBasicBlock &MBB,
                                 MachineBasicBlock::iterator MI,
                                 const DebugLoc &DL, Register DestReg,
                                 Register SrcReg, bool KillSrc,
                                 bool RenamableDest, bool RenamableSrc) const {
  if (!RVMin::GPRRegClass.contains(DestReg, SrcReg))
    llvm_unreachable("RVMin: only GPR-to-GPR copies are supported");

  BuildMI(MBB, MI, DL, get(RVMin::ADDI), DestReg)
      .addReg(SrcReg, getKillRegState(KillSrc))
      .addImm(0);
}

namespace {

bool isRVMinConditionalBranch(unsigned Opc) {
  switch (Opc) {
  case RVMin::BEQ:
  case RVMin::BNE:
  case RVMin::BLT:
  case RVMin::BGE:
  case RVMin::BLTU:
  case RVMin::BGEU:
    return true;
  default:
    return false;
  }
}

} // end anonymous namespace

bool RVMinInstrInfo::isUnconditionalBranch(const MachineInstr &MI) const {
  if (MI.getOpcode() == RVMin::PseudoBR)
    return true;
  // After pseudo expansion: `jal zero, target`.
  return MI.getOpcode() == RVMin::JAL && MI.getOperand(0).isReg() &&
         MI.getOperand(0).getReg() == RVMin::X0;
}

// BranchFolding and MachineBlockPlacement need to understand the control flow
// at the end of a block.  A block may end with:
//
//   - nothing (fall through),
//   - an unconditional PseudoBR / `jal zero`,
//   - one or two conditional branches (identical conditions may be merged).
//
// The condition is reconstructed in insertBranch from Cond =
// [opcode, rs1, rs2].
bool RVMinInstrInfo::analyzeBranch(MachineBasicBlock &MBB,
                                   MachineBasicBlock *&TBB,
                                   MachineBasicBlock *&FBB,
                                   SmallVectorImpl<MachineOperand> &Cond,
                                   bool AllowModify) const {
  TBB = nullptr;
  FBB = nullptr;
  Cond.clear();

  MachineBasicBlock::iterator I = MBB.end();
  while (I != MBB.begin()) {
    --I;
    if (I->isDebugInstr() || I->isCFIInstruction())
      continue;

    if (!isUnpredicatedTerminator(*I))
      return true;

    if (isUnconditionalBranch(*I)) {
      MachineBasicBlock *Dest = I->getOperand(I->getNumOperands() - 1).getMBB();
      if (!AllowModify) {
        TBB = Dest;
        continue;
      }

      // Anything after the branch is unreachable.
      MBB.erase(std::next(I), MBB.end());
      Cond.clear();
      FBB = nullptr;

      // A branch to the layout successor is a fall-through: delete it.
      if (MBB.isLayoutSuccessor(Dest)) {
        TBB = nullptr;
        I->eraseFromParent();
        I = MBB.end();
        continue;
      }

      TBB = Dest;
      continue;
    }

    if (!isRVMinConditionalBranch(I->getOpcode()))
      return true;
    if (!I->getOperand(I->getNumOperands() - 1).isMBB())
      return true;

    MachineBasicBlock *Dest = I->getOperand(I->getNumOperands() - 1).getMBB();
    if (Cond.empty()) {
      FBB = TBB;
      TBB = Dest;
      Cond.push_back(MachineOperand::CreateImm(I->getOpcode()));
      Cond.push_back(I->getOperand(0));
      Cond.push_back(I->getOperand(1));
      continue;
    }

    // Two conditional branches are only understood when they are identical;
    // this happens with e.g. `if (a == 0 || b)` after block merging.
    if (TBB != Dest || Cond[0].getImm() != I->getOpcode() ||
        !Cond[1].isIdenticalTo(I->getOperand(0)) ||
        !Cond[2].isIdenticalTo(I->getOperand(1)))
      return true;
  }

  return false;
}

unsigned RVMinInstrInfo::removeBranch(MachineBasicBlock &MBB,
                                      int *BytesRemoved) const {
  if (BytesRemoved)
    *BytesRemoved = 0;

  unsigned Count = 0;
  MachineBasicBlock::iterator I = MBB.end();
  while (I != MBB.begin()) {
    --I;
    if (I->isDebugInstr())
      continue;
    if (!isUnconditionalBranch(*I) && !isRVMinConditionalBranch(I->getOpcode()))
      break;
    if (BytesRemoved)
      *BytesRemoved += getInstSizeInBytes(*I);
    I->eraseFromParent();
    I = MBB.end();
    ++Count;
  }
  return Count;
}

unsigned RVMinInstrInfo::insertBranch(MachineBasicBlock &MBB,
                                      MachineBasicBlock *TBB,
                                      MachineBasicBlock *FBB,
                                      ArrayRef<MachineOperand> Cond,
                                      const DebugLoc &DL,
                                      int *BytesAdded) const {
  if (BytesAdded)
    *BytesAdded = 0;

  assert(TBB && "insertBranch must not be told to insert a fallthrough");
  assert((Cond.size() == 3 || Cond.empty()) &&
         "RVMin branch conditions have two register operands");

  if (Cond.empty()) {
    assert(!FBB && "unconditional branch with multiple successors");
    MachineInstr &MI = *BuildMI(&MBB, DL, get(RVMin::PseudoBR)).addMBB(TBB);
    if (BytesAdded)
      *BytesAdded += getInstSizeInBytes(MI);
    return 1;
  }

  MachineInstr &CondMI = *BuildMI(&MBB, DL, get(Cond[0].getImm()))
                              .add(Cond[1])
                              .add(Cond[2])
                              .addMBB(TBB);
  unsigned Count = 1;
  if (BytesAdded)
    *BytesAdded += getInstSizeInBytes(CondMI);

  if (FBB) {
    MachineInstr &MI = *BuildMI(&MBB, DL, get(RVMin::PseudoBR)).addMBB(FBB);
    if (BytesAdded)
      *BytesAdded += getInstSizeInBytes(MI);
    ++Count;
  }
  return Count;
}
