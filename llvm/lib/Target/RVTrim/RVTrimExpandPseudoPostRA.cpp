//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains one of the several passes that expand pseudo instructions
// into target instructions. This pass is run after register allocation and
// before post RA scheduling.
//
//===----------------------------------------------------------------------===//

#include "RVTrim.h"
#include "RVTrimExpandPseudoBase.h"
#include "RVTrimInstrInfo.h"
#include "RVTrimSubtarget.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"

using namespace llvm;

#define RVTrim_EXPAND_PSEUDO_POST_RA_NAME                                       \
  "RISC-V Pseudo Instruction Expansion - Post-RA"

namespace {

class RVTrimExpandPseudoPostRAImpl final : public RVTrimExpandPseudoImplBase {
  bool expandMI(MachineBasicBlock &MBB, MachineBasicBlock::iterator MBBI,
                MachineBasicBlock::iterator &NextMBBI) const override;

  bool expandMovImm(MachineBasicBlock &MBB,
                    MachineBasicBlock::iterator MBBI) const;

  bool expandMovAddr(MachineBasicBlock &MBB,
                     MachineBasicBlock::iterator MBBI) const;

  bool expandMERGE(MachineBasicBlock &MBB,
                   MachineBasicBlock::iterator MBBI) const;

  bool expandAddUpperImm(MachineBasicBlock &MBB,
                         MachineBasicBlock::iterator MBBI) const;
};

class RVTrimExpandPseudoPostRALegacy : public MachineFunctionPass {
public:
  static char ID;

  RVTrimExpandPseudoPostRALegacy() : MachineFunctionPass(ID) {}

  bool runOnMachineFunction(MachineFunction &MF) override {
    return RVTrimExpandPseudoPostRAImpl().run(MF);
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    MachineFunctionPass::getAnalysisUsage(AU);
  }

  StringRef getPassName() const override {
    return RVTrim_EXPAND_PSEUDO_POST_RA_NAME;
  }
};

} // anonymous namespace

bool RVTrimExpandPseudoPostRAImpl::expandMI(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MBBI,
    MachineBasicBlock::iterator &NextMBBI) const {
  switch (MBBI->getOpcode()) {
  case RVTrim::PseudoMovImm:
    return expandMovImm(MBB, MBBI);
  case RVTrim::PseudoMovAddr:
    return expandMovAddr(MBB, MBBI);
  case RVTrim::PseudoAddUpperImm:
    return expandAddUpperImm(MBB, MBBI);
  case RVTrim::PseudoMERGE:
    return expandMERGE(MBB, MBBI);
  }

  return false;
}

bool RVTrimExpandPseudoPostRAImpl::expandMovImm(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MBBI) const {
  DebugLoc DL = MBBI->getDebugLoc();

  int64_t Val = MBBI->getOperand(1).getImm();

  Register DstReg = MBBI->getOperand(0).getReg();
  bool DstIsDead = MBBI->getOperand(0).isDead();
  bool Renamable = MBBI->getOperand(0).isRenamable();

  TII->movImm(MBB, MBBI, DL, DstReg, Val, MachineInstr::NoFlags, Renamable,
              DstIsDead);

  MBBI->eraseFromParent();
  return true;
}

bool RVTrimExpandPseudoPostRAImpl::expandMovAddr(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MBBI) const {
  DebugLoc DL = MBBI->getDebugLoc();

  Register DstReg = MBBI->getOperand(0).getReg();
  bool DstIsDead = MBBI->getOperand(0).isDead();
  bool Renamable = MBBI->getOperand(0).isRenamable();

  BuildMI(MBB, MBBI, DL, TII->get(RVTrim::LUI))
      .addDef(DstReg, getRenamableRegState(Renamable))
      .add(MBBI->getOperand(1));
  BuildMI(MBB, MBBI, DL, TII->get(RVTrim::ADDI))
      .addDef(DstReg,
              getDeadRegState(DstIsDead) | getRenamableRegState(Renamable))
      .addReg(DstReg, RegState::Kill | getRenamableRegState(Renamable))
      .add(MBBI->getOperand(2));
  MBBI->eraseFromParent();
  return true;
}

bool RVTrimExpandPseudoPostRAImpl::expandAddUpperImm(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MBBI) const {
  DebugLoc DL = MBBI->getDebugLoc();

  Register DstReg = MBBI->getOperand(0).getReg();
  bool DstIsDead = MBBI->getOperand(0).isDead();
  bool Renamable = MBBI->getOperand(0).isRenamable();
  Register BaseReg = MBBI->getOperand(1).getReg();
  int64_t Hi = MBBI->getOperand(2).getImm();

  // Expand to LUI+ADD: the immediate is already the upper 20-bit value.
  BuildMI(MBB, MBBI, DL, TII->get(RVTrim::LUI))
      .addDef(DstReg, getRenamableRegState(Renamable))
      .addImm(Hi);
  BuildMI(MBB, MBBI, DL, TII->get(RVTrim::ADD))
      .addDef(DstReg,
              getDeadRegState(DstIsDead) | getRenamableRegState(Renamable))
      .addReg(BaseReg)
      .addReg(DstReg, RegState::Kill | getRenamableRegState(Renamable));

  MBBI->eraseFromParent();
  return true;
}

/// Transfer implicit operands on the pseudo instruction to the
/// instructions created from the expansion.
static void transferImpOps(const MachineInstr &OldMI, MachineInstrBuilder &MI) {
  const MCInstrDesc &Desc = OldMI.getDesc();
  for (const MachineOperand &MO :
       llvm::drop_begin(OldMI.operands(), Desc.getNumOperands())) {
    assert(MO.isReg() && MO.getReg());
    MI.add(MO);
  }
}

// Expand PseudoMERGE to MERGE, MVM, or MVMN.
bool RVTrimExpandPseudoPostRAImpl::expandMERGE(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MBBI) const {
  MachineInstr &MI = *MBBI;
  DebugLoc DL = MI.getDebugLoc();

  Register DstReg = MI.getOperand(0).getReg();
  if (DstReg == MI.getOperand(3).getReg()) {
    // Expand to MVMN
    auto I = BuildMI(MBB, MBBI, DL, TII->get(RVTrim::MVMN))
                 .add(MI.getOperand(0))
                 .add(MI.getOperand(3))
                 .add(MI.getOperand(2))
                 .add(MI.getOperand(1));
    transferImpOps(*MBBI, I);
  } else if (DstReg == MBBI->getOperand(2).getReg()) {
    // Expand to MVM
    auto I = BuildMI(MBB, MBBI, DL, TII->get(RVTrim::MVM))
                 .add(MI.getOperand(0))
                 .add(MI.getOperand(2))
                 .add(MI.getOperand(3))
                 .add(MI.getOperand(1));
    transferImpOps(*MBBI, I);
  } else if (DstReg == MI.getOperand(1).getReg()) {
    // Expand to MERGE
    auto I = BuildMI(MBB, MBBI, DL, TII->get(RVTrim::MERGE))
                 .add(MI.getOperand(0))
                 .add(MI.getOperand(1))
                 .add(MI.getOperand(2))
                 .add(MI.getOperand(3));
    transferImpOps(*MBBI, I);
  } else {
    // Use an additional move.
    RegState RegState =
        getRenamableRegState(MI.getOperand(1).isRenamable()) |
        getKillRegState(MI.getOperand(1).isKill() &&
                        MI.getOperand(1).getReg() !=
                            MI.getOperand(2).getReg() &&
                        MI.getOperand(1).getReg() != MI.getOperand(3).getReg());
    BuildMI(MBB, MBBI, MI.getDebugLoc(), TII->get(RVTrim::ADDI))
        .addDef(DstReg, getRenamableRegState(MI.getOperand(0).isRenamable()))
        .addReg(MI.getOperand(1).getReg(), RegState)
        .addImm(0);
    auto I = BuildMI(MBB, MBBI, MI.getDebugLoc(), TII->get(RVTrim::MERGE))
                 .add(MI.getOperand(0))
                 .addReg(DstReg,
                         RegState::Kill | getRenamableRegState(
                                              MI.getOperand(0).isRenamable()))
                 .add(MI.getOperand(2))
                 .add(MI.getOperand(3));
    transferImpOps(*MBBI, I);
  }
  MI.eraseFromParent();
  return true;
}

char RVTrimExpandPseudoPostRALegacy::ID = 0;

INITIALIZE_PASS(RVTrimExpandPseudoPostRALegacy, "rvtrim-expand-pseudo-post-ra",
                RVTrim_EXPAND_PSEUDO_POST_RA_NAME, false, false)

FunctionPass *llvm::createRVTrimExpandPseudoPostRALegacyPass() {
  return new RVTrimExpandPseudoPostRALegacy();
}

PreservedAnalyses
RVTrimExpandPseudoPostRAPass::run(MachineFunction &MF,
                                 MachineFunctionAnalysisManager &MFAM) {
  bool Changed = RVTrimExpandPseudoPostRAImpl().run(MF);
  if (!Changed)
    return PreservedAnalyses::all();

  PreservedAnalyses PA = getMachineFunctionPassPreservedAnalyses();
  PA.preserveSet<CFGAnalyses>();
  return PA;
}
