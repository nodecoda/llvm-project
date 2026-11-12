//=- RVTrimRedundantCopyElimination.cpp - Remove useless copy for RISC-V -----=//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This pass removes unnecessary zero copies in BBs that are targets of
// beqz/bnez instructions. For instance, the copy instruction in the code below
// can be removed because the beqz jumps to BB#2 when a0 is zero.
//  BB#1:
//    beqz %a0, <BB#2>
//  BB#2:
//    %a0 = COPY %x0
//
// This pass also recognizes Xqcibi branch-immediate forms when compared
// against non-zero immediates.
//
// This pass should be run after register allocation and is based on the
// earliest versions of AArch64RedundantCopyElimination.
//
// The pass also handles register-register branches when one operand is
// materialized as a non-zero immediate in the predecessor block.
//
//===----------------------------------------------------------------------===//

#include "RVTrim.h"
#include "RVTrimInstrInfo.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/MachineFunctionAnalysisManager.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachinePassManager.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/RegisterClassInfo.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/IR/Analysis.h"
#include "llvm/Support/Debug.h"
#include <optional>

using namespace llvm;

#define DEBUG_TYPE "rvtrim-redundant-copy-elimination"

STATISTIC(NumCopiesRemoved, "Number of copies removed.");

namespace {
class RVTrimRedundantCopyEliminationLegacy : public MachineFunctionPass {
public:
  static char ID;
  RVTrimRedundantCopyEliminationLegacy() : MachineFunctionPass(ID) {}

  bool runOnMachineFunction(MachineFunction &MF) override;
  MachineFunctionProperties getRequiredProperties() const override {
    return MachineFunctionProperties().setNoVRegs();
  }

  StringRef getPassName() const override {
    return "RISC-V Redundant Copy Elimination";
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    AU.addPreserved<MachineRegisterClassInfoWrapperPass>();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
};

} // end anonymous namespace

char RVTrimRedundantCopyEliminationLegacy::ID = 0;

INITIALIZE_PASS(RVTrimRedundantCopyEliminationLegacy, "rvtrim-copyelim",
                "RISC-V Redundant Copy Elimination", false, false)

static bool
guaranteesZeroRegInBlock(MachineBasicBlock &MBB,
                         const SmallVectorImpl<MachineOperand> &Cond,
                         MachineBasicBlock *TBB) {
  assert(Cond.size() == 3 && "Unexpected number of operands");
  assert(TBB != nullptr && "Expected branch target basic block");
  auto Opc = Cond[0].getImm();
  if (Opc == RVTrim::BEQ && Cond[2].isReg() && Cond[2].getReg() == RVTrim::X0 &&
      TBB == &MBB)
    return true;
  if (Opc == RVTrim::BNE && Cond[2].isReg() && Cond[2].getReg() == RVTrim::X0 &&
      TBB != &MBB)
    return true;
  return false;
}

static bool
guaranteesRegEqualsImmInBlock(MachineBasicBlock &MBB,
                              const SmallVectorImpl<MachineOperand> &Cond,
                              MachineBasicBlock *TBB) {
  assert(Cond.size() == 3 && "Unexpected number of operands");
  assert(TBB != nullptr && "Expected branch target basic block");
  auto Opc = Cond[0].getImm();
  if ((Opc == RVTrim::QC_BEQI || Opc == RVTrim::QC_E_BEQI ||
       Opc == RVTrim::NDS_BEQC || Opc == RVTrim::BEQI) &&
      Cond[2].isImm() && Cond[2].getImm() != 0 && TBB == &MBB)
    return true;
  if ((Opc == RVTrim::QC_BNEI || Opc == RVTrim::QC_E_BNEI ||
       Opc == RVTrim::NDS_BNEC || Opc == RVTrim::BNEI) &&
      Cond[2].isImm() && Cond[2].getImm() != 0 && TBB != &MBB)
    return true;
  return false;
}

// Match copy from x0, "addi rd, x0, imm", or "qc.li rd, imm", returning the
// defined register and the known value. Reg is invalid if MI isn't a match.
static RegImmPair matchRegKnownVal(const MachineInstr &MI) {
  if (MI.isCopy() && MI.getOperand(0).isReg() && MI.getOperand(1).isReg() &&
      MI.getOperand(1).getReg() == RVTrim::X0)
    return RegImmPair(MI.getOperand(0).getReg(), 0);
  if (MI.getOpcode() == RVTrim::ADDI && MI.getOperand(0).isReg() &&
      MI.getOperand(1).isReg() && MI.getOperand(1).getReg() == RVTrim::X0 &&
      MI.getOperand(2).isImm())
    return RegImmPair(MI.getOperand(0).getReg(), MI.getOperand(2).getImm());
  if (MI.getOpcode() == RVTrim::QC_LI && MI.getOperand(0).isReg() &&
      MI.getOperand(1).isImm())
    return RegImmPair(MI.getOperand(0).getReg(), MI.getOperand(1).getImm());
  return RegImmPair(Register(), 0);
}

static std::optional<int64_t>
getRegImmediateBeforeTerminator(MachineBasicBlock &MBB, Register Reg,
                                const TargetRegisterInfo *TRI) {
  // A write to X0 is discarded, so it cannot establish a nonzero value.
  if (Reg == RVTrim::X0)
    return std::nullopt;

  for (auto I = MBB.getFirstTerminator(); I != MBB.begin();) {
    MachineInstr &MI = *--I;
    if (!MI.modifiesRegister(Reg, TRI))
      continue;
    // The last modification must define Reg itself to a known value.
    RegImmPair Match = matchRegKnownVal(MI);
    if (Match.Reg == Reg)
      return Match.Imm;
    return std::nullopt;
  }
  return std::nullopt;
}

static bool optimizeBlockForRedundantCopies(MachineBasicBlock &MBB,
                                            const MachineRegisterInfo *MRI,
                                            const TargetRegisterInfo *TRI,
                                            const TargetInstrInfo *TII) {
  // Check if the current basic block has a single predecessor.
  if (MBB.pred_size() != 1)
    return false;

  // Check if the predecessor has two successors, implying the block ends in a
  // conditional branch.
  MachineBasicBlock *PredMBB = *MBB.pred_begin();
  if (PredMBB->succ_size() != 2)
    return false;

  MachineBasicBlock *TBB = nullptr, *FBB = nullptr;
  SmallVector<MachineOperand, 3> Cond;
  if (TII->analyzeBranch(*PredMBB, TBB, FBB, Cond, /*AllowModify*/ false) ||
      Cond.empty())
    return false;

  Register TargetReg = Cond[1].getReg();

  if (!TargetReg)
    return false;

  bool IsZeroCopy = guaranteesZeroRegInBlock(MBB, Cond, TBB);
  bool IsImmCopy = !IsZeroCopy && guaranteesRegEqualsImmInBlock(MBB, Cond, TBB);
  int64_t CompareImm = IsImmCopy ? Cond[2].getImm() : 0;
  if (!IsZeroCopy && !IsImmCopy && Cond.size() == 3 &&
      (Cond[0].getImm() == RVTrim::BEQ || Cond[0].getImm() == RVTrim::BNE) &&
      Cond[2].isReg()) {
    // One branch operand may have been materialized with ADDI or QC_LI.
    // The other operand is known to have the same value on the equality edge,
    // irrespective of the operand order.
    std::optional<int64_t> Imm =
        getRegImmediateBeforeTerminator(*PredMBB, Cond[2].getReg(), TRI);
    if (Imm && *Imm != 0) {
      TargetReg = Cond[1].getReg();
      CompareImm = *Imm;
      IsImmCopy = true;
    } else {
      Imm = getRegImmediateBeforeTerminator(*PredMBB, Cond[1].getReg(), TRI);
      if (Imm && *Imm != 0) {
        TargetReg = Cond[2].getReg();
        CompareImm = *Imm;
        IsImmCopy = true;
      }
    }
    // For BEQ, equality is guaranteed on the taken edge. For BNE, it is
    // guaranteed on the fallthrough edge.
    IsImmCopy &= (Cond[0].getImm() == RVTrim::BEQ) == (TBB == &MBB);
  }

  if (!IsZeroCopy && !IsImmCopy)
    return false;

  bool Changed = false;
  MachineBasicBlock::iterator LastChange = MBB.begin();
  // Remove redundant Copy instructions unless TargetReg is modified.
  for (MachineBasicBlock::iterator I = MBB.begin(), E = MBB.end(); I != E;) {
    MachineInstr *MI = &*I;
    ++I;
    // Compare with non-zero immediate or a known register value:
    // remove redundant copy, addi rd,x0,imm, or qc.li rd,imm as applicable.
    RegImmPair Match = matchRegKnownVal(*MI);
    if (Match.Reg && TargetReg == Match.Reg && Match.Imm == CompareImm &&
        !MRI->isReserved(Match.Reg)) {
      LLVM_DEBUG(dbgs() << "Remove redundant Copy: ");
      LLVM_DEBUG(MI->print(dbgs()));

      MI->eraseFromParent();
      Changed = true;
      LastChange = I;
      ++NumCopiesRemoved;
      continue;
    }

    if (MI->modifiesRegister(TargetReg, TRI))
      break;
  }

  if (!Changed)
    return false;

  MachineBasicBlock::iterator CondBr = PredMBB->getFirstTerminator();
  assert((CondBr->getOpcode() == RVTrim::BEQ ||
          CondBr->getOpcode() == RVTrim::BNE ||
          CondBr->getOpcode() == RVTrim::BEQI ||
          CondBr->getOpcode() == RVTrim::BNEI ||
          CondBr->getOpcode() == RVTrim::QC_BEQI ||
          CondBr->getOpcode() == RVTrim::QC_BNEI ||
          CondBr->getOpcode() == RVTrim::QC_E_BEQI ||
          CondBr->getOpcode() == RVTrim::QC_E_BNEI ||
          CondBr->getOpcode() == RVTrim::NDS_BEQC ||
          CondBr->getOpcode() == RVTrim::NDS_BNEC) &&
         "Unexpected opcode");
  assert((CondBr->getOperand(0).getReg() == TargetReg ||
          CondBr->getOperand(1).getReg() == TargetReg) &&
         "Unexpected register");

  // Otherwise, we have to fixup the use-def chain, starting with the
  // BEQ(I)/BNE(I). Conservatively mark as much as we can live.
  CondBr->clearRegisterKills(TargetReg, TRI);

  // Add newly used reg to the block's live-in list if it isn't there already.
  if (!MBB.isLiveIn(TargetReg))
    MBB.addLiveIn(TargetReg);

  // Clear any kills of TargetReg between CondBr and the last removed COPY.
  for (MachineInstr &MMI : make_range(MBB.begin(), LastChange))
    MMI.clearRegisterKills(TargetReg, TRI);

  return true;
}

static bool runRedundantCopyElimination(MachineFunction &MF) {
  const TargetInstrInfo *TII = MF.getSubtarget().getInstrInfo();
  const TargetRegisterInfo *TRI = MF.getSubtarget().getRegisterInfo();
  const MachineRegisterInfo *MRI = &MF.getRegInfo();

  bool Changed = false;
  for (MachineBasicBlock &MBB : MF)
    Changed |= optimizeBlockForRedundantCopies(MBB, MRI, TRI, TII);

  return Changed;
}

FunctionPass *llvm::createRVTrimRedundantCopyEliminationLegacyPass() {
  return new RVTrimRedundantCopyEliminationLegacy();
}

bool RVTrimRedundantCopyEliminationLegacy::runOnMachineFunction(
    MachineFunction &MF) {
  if (skipFunction(MF.getFunction()))
    return false;
  return runRedundantCopyElimination(MF);
}

PreservedAnalyses
RVTrimRedundantCopyEliminationPass::run(MachineFunction &MF,
                                       MachineFunctionAnalysisManager &MFAM) {
  MFPropsModifier _(*this, MF);
  bool Changed = runRedundantCopyElimination(MF);
  if (!Changed)
    return PreservedAnalyses::all();
  return getMachineFunctionPassPreservedAnalyses()
      .preserveSet<CFGAnalyses>()
      .preserve<MachineRegisterClassAnalysis>();
}
