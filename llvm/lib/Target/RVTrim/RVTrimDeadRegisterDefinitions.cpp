//===- RVTrimDeadRegisterDefinitions.cpp - Replace dead defs w/ zero reg --===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===---------------------------------------------------------------------===//
//
// This pass rewrites Rd to x0 for instrs whose return values are unused.
//
//===---------------------------------------------------------------------===//

#include "RVTrim.h"
#include "RVTrimSubtarget.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/LiveDebugVariables.h"
#include "llvm/CodeGen/LiveIntervals.h"
#include "llvm/CodeGen/LiveStacks.h"
#include "llvm/CodeGen/MachineFunctionAnalysisManager.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachinePassManager.h"
#include "llvm/CodeGen/SlotIndexes.h"
#include "llvm/IR/Analysis.h"

using namespace llvm;
#define DEBUG_TYPE "rvtrim-dead-defs"
#define RVTrim_DEAD_REG_DEF_NAME "RISC-V Dead register definitions"

STATISTIC(NumDeadDefsReplaced, "Number of dead definitions replaced");

namespace {
class RVTrimDeadRegisterDefinitionsLegacy : public MachineFunctionPass {
public:
  static char ID;

  RVTrimDeadRegisterDefinitionsLegacy() : MachineFunctionPass(ID) {}
  bool runOnMachineFunction(MachineFunction &MF) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    AU.addRequired<LiveIntervalsWrapperPass>();
    AU.addPreserved<LiveIntervalsWrapperPass>();
    AU.addPreserved<SlotIndexesWrapperPass>();
    AU.addPreserved<LiveDebugVariablesWrapperLegacy>();
    AU.addPreserved<LiveStacksWrapperLegacy>();
    MachineFunctionPass::getAnalysisUsage(AU);
  }

  StringRef getPassName() const override { return RVTrim_DEAD_REG_DEF_NAME; }
};
} // end anonymous namespace

char RVTrimDeadRegisterDefinitionsLegacy::ID = 0;
INITIALIZE_PASS(RVTrimDeadRegisterDefinitionsLegacy, DEBUG_TYPE,
                RVTrim_DEAD_REG_DEF_NAME, false, false)

FunctionPass *llvm::createRVTrimDeadRegisterDefinitionsLegacyPass() {
  return new RVTrimDeadRegisterDefinitionsLegacy();
}

static bool runDeadRegisterDefinitions(MachineFunction &MF,
                                       function_ref<LiveIntervals &()> GetLIS) {
  const TargetInstrInfo *TII = MF.getSubtarget().getInstrInfo();
  LiveIntervals &LIS = GetLIS();
  LLVM_DEBUG(dbgs() << "***** RVTrimDeadRegisterDefinitions *****\n");

  bool MadeChange = false;
  for (MachineBasicBlock &MBB : MF) {
    for (MachineInstr &MI : MBB) {
      // We only handle non-computational instructions since some NOP encodings
      // are reserved for HINT instructions.
      const MCInstrDesc &Desc = MI.getDesc();
      if (!Desc.mayLoad() && !Desc.mayStore() &&
          !Desc.hasUnmodeledSideEffects() &&
          MI.getOpcode() != RVTrim::PseudoVSETVLI &&
          MI.getOpcode() != RVTrim::PseudoVSETIVLI)
        continue;
      for (int I = 0, E = Desc.getNumDefs(); I != E; ++I) {
        MachineOperand &MO = MI.getOperand(I);
        if (!MO.isReg() || !MO.isDef() || MO.isEarlyClobber())
          continue;
        // Be careful not to change the register if it's a tied operand.
        if (MI.isRegTiedToUseOperand(I)) {
          LLVM_DEBUG(dbgs() << "    Ignoring, def is tied operand.\n");
          continue;
        }
        Register Reg = MO.getReg();
        if (!Reg.isVirtual() || !MO.isDead())
          continue;
        LLVM_DEBUG(dbgs() << "    Dead def operand #" << I << " in:\n      ";
                   MI.print(dbgs()));
        Register X0Reg;
        const TargetRegisterClass *RC = TII->getRegClass(Desc, I);
        if (RC && RC->contains(RVTrim::X0)) {
          X0Reg = RVTrim::X0;
        } else if (RC && RC->contains(RVTrim::X0_W)) {
          X0Reg = RVTrim::X0_W;
        } else if (RC && RC->contains(RVTrim::X0_H)) {
          X0Reg = RVTrim::X0_H;
        } else if (RC && RC->contains(RVTrim::X0_Pair)) {
          X0Reg = RVTrim::X0_Pair;
        } else {
          LLVM_DEBUG(dbgs() << "    Ignoring, register is not a GPR.\n");
          continue;
        }
        assert(LIS.hasInterval(Reg));
        LIS.removeInterval(Reg);
        MO.setReg(X0Reg);
        LLVM_DEBUG(dbgs() << "    Replacing with zero register. New:\n      ";
                   MI.print(dbgs()));
        ++NumDeadDefsReplaced;
        MadeChange = true;
      }
    }
  }

  return MadeChange;
}

bool RVTrimDeadRegisterDefinitionsLegacy::runOnMachineFunction(
    MachineFunction &MF) {
  if (skipFunction(MF.getFunction()))
    return false;
  return runDeadRegisterDefinitions(MF, [&]() -> LiveIntervals & {
    return getAnalysis<LiveIntervalsWrapperPass>().getLIS();
  });
}

PreservedAnalyses
RVTrimDeadRegisterDefinitionsPass::run(MachineFunction &MF,
                                      MachineFunctionAnalysisManager &MFAM) {
  bool Changed = runDeadRegisterDefinitions(MF, [&]() -> LiveIntervals & {
    return MFAM.getResult<LiveIntervalsAnalysis>(MF);
  });
  if (!Changed)
    return PreservedAnalyses::all();
  return getMachineFunctionPassPreservedAnalyses()
      .preserveSet<CFGAnalyses>()
      .preserve<LiveIntervalsAnalysis>()
      .preserve<SlotIndexesAnalysis>()
      .preserve<LiveDebugVariablesAnalysis>()
      .preserve<LiveStacksAnalysis>();
}
