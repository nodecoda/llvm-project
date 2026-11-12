//===-- RVTrimInsertReadWriteCSR.cpp - Insert Read/Write of RISC-V CSR -----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
// This file implements the machine function pass to insert read/write of CSR-s
// of the RISC-V instructions.
//
// Currently the pass implements:
// -Writing and saving frm before an RVV floating-point instruction with a
//  static rounding mode and restores the value after.
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/RVTrimBaseInfo.h"
#include "RVTrim.h"
#include "RVTrimSubtarget.h"
#include "llvm/CodeGen/MachineFunctionAnalysisManager.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachinePassManager.h"
#include "llvm/IR/Analysis.h"
using namespace llvm;

#define DEBUG_TYPE "rvtrim-insert-read-write-csr"
#define RVTrim_INSERT_READ_WRITE_CSR_NAME "RISC-V Insert Read/Write CSR Pass"

namespace {

class RVTrimInsertReadWriteCSRLegacy : public MachineFunctionPass {
public:
  static char ID;

  RVTrimInsertReadWriteCSRLegacy() : MachineFunctionPass(ID) {}

  bool runOnMachineFunction(MachineFunction &MF) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    MachineFunctionPass::getAnalysisUsage(AU);
  }

  StringRef getPassName() const override {
    return RVTrim_INSERT_READ_WRITE_CSR_NAME;
  }
};

} // end anonymous namespace

char RVTrimInsertReadWriteCSRLegacy::ID = 0;

INITIALIZE_PASS(RVTrimInsertReadWriteCSRLegacy, DEBUG_TYPE,
                RVTrim_INSERT_READ_WRITE_CSR_NAME, false, false)

// TODO: Use more accurate rounding mode at the start of MBB.
static bool emitWriteRoundingModeOpt(MachineBasicBlock &MBB,
                                     const TargetInstrInfo *TII) {
  bool Changed = false;
  MachineInstr *LastFRMChanger = nullptr;
  unsigned CurrentRM = RVTrimFPRndMode::DYN;
  Register SavedFRM;

  for (MachineInstr &MI : MBB) {
    if (MI.getOpcode() == RVTrim::SwapFRMImm ||
        MI.getOpcode() == RVTrim::WriteFRMImm) {
      CurrentRM = MI.getOperand(0).getImm();
      SavedFRM = Register();
      continue;
    }

    if (MI.getOpcode() == RVTrim::WriteFRM) {
      CurrentRM = RVTrimFPRndMode::DYN;
      SavedFRM = Register();
      continue;
    }

    if (MI.isCall() || MI.isInlineAsm() ||
        MI.readsRegister(RVTrim::FRM, /*TRI=*/nullptr)) {
      // Restore FRM before unknown operations.
      if (SavedFRM.isValid())
        BuildMI(MBB, MI, MI.getDebugLoc(), TII->get(RVTrim::WriteFRM))
            .addReg(SavedFRM);
      CurrentRM = RVTrimFPRndMode::DYN;
      SavedFRM = Register();
      continue;
    }

    assert(!MI.modifiesRegister(RVTrim::FRM, /*TRI=*/nullptr) &&
           "Expected that MI could not modify FRM.");

    int FRMIdx = RVTrimII::getFRMOpNum(MI.getDesc());
    if (FRMIdx < 0)
      continue;
    unsigned InstrRM = MI.getOperand(FRMIdx).getImm();

    LastFRMChanger = &MI;

    // Make MI implicit use FRM.
    MI.addOperand(MachineOperand::CreateReg(RVTrim::FRM, /*IsDef*/ false,
                                            /*IsImp*/ true));
    Changed = true;

    // Skip if MI uses same rounding mode as FRM.
    if (InstrRM == CurrentRM)
      continue;

    if (!SavedFRM.isValid()) {
      // Save current FRM value to SavedFRM.
      MachineRegisterInfo *MRI = &MBB.getParent()->getRegInfo();
      SavedFRM = MRI->createVirtualRegister(&RVTrim::GPRRegClass);
      BuildMI(MBB, MI, MI.getDebugLoc(), TII->get(RVTrim::SwapFRMImm), SavedFRM)
          .addImm(InstrRM);
    } else {
      // Don't need to save current FRM when SavedFRM having value.
      BuildMI(MBB, MI, MI.getDebugLoc(), TII->get(RVTrim::WriteFRMImm))
          .addImm(InstrRM);
    }
    CurrentRM = InstrRM;
  }

  // Restore FRM if needed.
  if (SavedFRM.isValid()) {
    assert(LastFRMChanger && "Expected valid pointer.");
    MachineInstrBuilder MIB =
        BuildMI(*MBB.getParent(), {}, TII->get(RVTrim::WriteFRM))
            .addReg(SavedFRM);
    MBB.insertAfter(LastFRMChanger, MIB);
  }

  return Changed;
}

// This function also swaps frm and restores it when encountering an RVV
// floating point instruction with a static rounding mode.
static bool emitWriteRoundingMode(MachineBasicBlock &MBB,
                                  const TargetInstrInfo *TII) {
  bool Changed = false;
  for (MachineInstr &MI : MBB) {
    int FRMIdx = RVTrimII::getFRMOpNum(MI.getDesc());
    if (FRMIdx < 0)
      continue;

    unsigned FRMImm = MI.getOperand(FRMIdx).getImm();

    // The value is a hint to this pass to not alter the frm value.
    if (FRMImm == RVTrimFPRndMode::DYN)
      continue;

    Changed = true;

    // Save
    MachineRegisterInfo *MRI = &MBB.getParent()->getRegInfo();
    Register SavedFRM = MRI->createVirtualRegister(&RVTrim::GPRRegClass);
    BuildMI(MBB, MI, MI.getDebugLoc(), TII->get(RVTrim::SwapFRMImm),
            SavedFRM)
        .addImm(FRMImm);
    MI.addOperand(MachineOperand::CreateReg(RVTrim::FRM, /*IsDef*/ false,
                                            /*IsImp*/ true));
    // Restore
    MachineInstrBuilder MIB =
        BuildMI(*MBB.getParent(), {}, TII->get(RVTrim::WriteFRM))
            .addReg(SavedFRM);
    MBB.insertAfter(MI, MIB);
  }
  return Changed;
}

static bool insertReadWriteCSROnMachineFunction(MachineFunction &MF) {
  // Skip if the vector extension is not enabled.
  const RVTrimSubtarget &ST = MF.getSubtarget<RVTrimSubtarget>();
  if (!ST.hasVInstructions())
    return false;

  const TargetInstrInfo *TII = ST.getInstrInfo();

  bool Changed = false;

  for (MachineBasicBlock &MBB : MF) {
    if (!ST.getCLOpts().frm_insert_opt)
      Changed |= emitWriteRoundingMode(MBB, TII);
    else
      Changed |= emitWriteRoundingModeOpt(MBB, TII);
  }

  return Changed;
}

FunctionPass *llvm::createRVTrimInsertReadWriteCSRLegacyPass() {
  return new RVTrimInsertReadWriteCSRLegacy();
}

bool RVTrimInsertReadWriteCSRLegacy::runOnMachineFunction(MachineFunction &MF) {
  return insertReadWriteCSROnMachineFunction(MF);
}

PreservedAnalyses
RVTrimInsertReadWriteCSRPass::run(MachineFunction &MF,
                                 MachineFunctionAnalysisManager &MFAM) {
  return insertReadWriteCSROnMachineFunction(MF)
             ? getMachineFunctionPassPreservedAnalyses()
                   .preserveSet<CFGAnalyses>()
             : PreservedAnalyses::all();
}
