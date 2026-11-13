//===-- MiniRVInstrInfo.cpp - MiniRV Instruction Information --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "MiniRVInstrInfo.h"
#include "MiniRVSubtarget.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/Target/TargetMachine.h"

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

unsigned MiniRVInstrInfo::getInstSizeInBytes(const MachineInstr &MI) const {
  if (MI.isMetaInstruction())
    return 0;

  switch (MI.getOpcode()) {
  case TargetOpcode::INLINEASM:
  case TargetOpcode::INLINEASM_BR: {
    const MachineFunction &MF = *MI.getParent()->getParent();
    return getInlineAsmLength(MI.getOperand(0).getSymbolName(),
                              MF.getTarget().getMCAsmInfo());
  }
  case TargetOpcode::BUNDLE:
    return getInstBundleSize(MI);
  case MiniRV::PseudoLA:
    // Expanded by the AsmPrinter into auipc + addi.
    return 8;
  case MiniRV::ADJCALLSTACKDOWN:
  case MiniRV::ADJCALLSTACKUP:
    return 0;
  default:
    // Every real MiniRV instruction is 32 bits wide; the remaining pseudos
    // expand to exactly one of them.
    return get(MI.getOpcode()).getSize();
  }
}

MachineBasicBlock *
MiniRVInstrInfo::getBranchDestBlock(const MachineInstr &MI) const {
  assert(MI.getDesc().isBranch() && "Unexpected opcode!");
  // Every MiniRV branch and jump carries its target as the last operand.
  return MI.getOperand(MI.getNumExplicitOperands() - 1).getMBB();
}

bool MiniRVInstrInfo::isBranchOffsetInRange(unsigned BranchOp,
                                            int64_t BrOffset) const {
  switch (BranchOp) {
  case MiniRV::BEQ:
  case MiniRV::BNE:
  case MiniRV::BLT:
  case MiniRV::BGE:
  case MiniRV::BLTU:
  case MiniRV::BGEU:
    return isInt<13>(BrOffset);
  case MiniRV::JAL:
  case MiniRV::PseudoBR:
    return isInt<21>(BrOffset);
  default:
    llvm_unreachable("Unexpected opcode!");
  }
}

bool MiniRVInstrInfo::reverseBranchCondition(
    SmallVectorImpl<MachineOperand> &Cond) const {
  assert(Cond.size() == 3 && "Invalid branch condition!");
  unsigned Inverse;
  switch (Cond[0].getImm()) {
  case MiniRV::BEQ:
    Inverse = MiniRV::BNE;
    break;
  case MiniRV::BNE:
    Inverse = MiniRV::BEQ;
    break;
  case MiniRV::BLT:
    Inverse = MiniRV::BGE;
    break;
  case MiniRV::BGE:
    Inverse = MiniRV::BLT;
    break;
  case MiniRV::BLTU:
    Inverse = MiniRV::BGEU;
    break;
  case MiniRV::BGEU:
    Inverse = MiniRV::BLTU;
    break;
  default:
    return true;
  }
  Cond[0].setImm(Inverse);
  return false;
}

void MiniRVInstrInfo::insertIndirectBranch(MachineBasicBlock &MBB,
                                           MachineBasicBlock &DestBB,
                                           MachineBasicBlock &RestoreBB,
                                           const DebugLoc &DL,
                                           int64_t BrOffset,
                                           RegScavenger *RS) const {
  assert(MBB.empty() && "new block should be inserted for expanding "
                        "unconditional branch");
  assert(RestoreBB.empty() && "restore block should be inserted for restoring "
                              "clobbered registers");
  if (!isInt<32>(BrOffset))
    report_fatal_error(
        "branch offsets outside the signed 32-bit range are not supported");

  // The reserved scratch register holds the address of the destination, so
  // nothing live is clobbered and RestoreBB stays empty.
  BuildMI(MBB, MBB.end(), DL, get(MiniRV::PseudoLA), MiniRV::X5)
      .addMBB(&DestBB);
  BuildMI(MBB, MBB.end(), DL, get(MiniRV::PseudoBRIND)).addReg(MiniRV::X5);
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
      .addImm(0)
      .setMIFlag(Flags);
}

void MiniRVInstrInfo::loadRegFromStackSlot(MachineBasicBlock &MBB,
                                           MachineBasicBlock::iterator MI,
                                           Register DestReg, int FrameIndex,
                                           const TargetRegisterClass *RC,
                                           Register VReg, unsigned SubReg,
                                           MachineInstr::MIFlag Flags) const {
  BuildMI(MBB, MI, MBB.findDebugLoc(MI), get(MiniRV::LD), DestReg)
      .addFrameIndex(FrameIndex)
      .addImm(0)
      .setMIFlag(Flags);
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
