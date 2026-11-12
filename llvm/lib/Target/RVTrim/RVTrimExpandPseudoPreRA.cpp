//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains one of the several passes that expand pseudo instructions
// into target instructions. This pass is run before register allocation.
//
//===----------------------------------------------------------------------===//

#include "RVTrim.h"
#include "RVTrimExpandPseudoBase.h"
#include "RVTrimInstrInfo.h"
#include "RVTrimSubtarget.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/MC/MCContext.h"

using namespace llvm;

#define RVTrim_EXPAND_PSEUDO_PRE_RA_NAME                                        \
  "RISC-V Pseudo Instruction Expansion - Pre-RA"

namespace {

class RVTrimExpandPseudoPreRAImpl final : public RVTrimExpandPseudoImplBase {
  bool expandMI(MachineBasicBlock &MBB, MachineBasicBlock::iterator MBBI,
                MachineBasicBlock::iterator &NextMBBI) const override;

  bool expandAuipcInstPair(MachineBasicBlock &MBB,
                           MachineBasicBlock::iterator MBBI, unsigned FlagsHi,
                           unsigned SecondOpcode) const;

  bool expandLoadLocalAddress(MachineBasicBlock &MBB,
                              MachineBasicBlock::iterator MBBI) const;

  bool expandLoadGlobalAddress(MachineBasicBlock &MBB,
                               MachineBasicBlock::iterator MBBI) const;

  bool expandLoadTLSIEAddress(MachineBasicBlock &MBB,
                              MachineBasicBlock::iterator MBBI) const;

  bool expandLoadTLSGDAddress(MachineBasicBlock &MBB,
                              MachineBasicBlock::iterator MBBI) const;

  bool expandLoadTLSDescAddress(MachineBasicBlock &MBB,
                                MachineBasicBlock::iterator MBBI) const;
};

class RVTrimExpandPseudoPreRALegacy : public MachineFunctionPass {
public:
  static char ID;

  RVTrimExpandPseudoPreRALegacy() : MachineFunctionPass(ID) {}

  bool runOnMachineFunction(MachineFunction &MF) override {
    return RVTrimExpandPseudoPreRAImpl().run(MF);
  }

  MachineFunctionProperties getRequiredProperties() const override {
    return MachineFunctionProperties().setIsSSA();
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
  StringRef getPassName() const override {
    return RVTrim_EXPAND_PSEUDO_PRE_RA_NAME;
  }
};

} // anonymous namespace

bool RVTrimExpandPseudoPreRAImpl::expandMI(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MBBI,
    MachineBasicBlock::iterator &NextMBBI) const {

  switch (MBBI->getOpcode()) {
  case RVTrim::PseudoLLA:
    return expandLoadLocalAddress(MBB, MBBI);
  case RVTrim::PseudoLGA:
    return expandLoadGlobalAddress(MBB, MBBI);
  case RVTrim::PseudoLA_TLS_IE:
    return expandLoadTLSIEAddress(MBB, MBBI);
  case RVTrim::PseudoLA_TLS_GD:
    return expandLoadTLSGDAddress(MBB, MBBI);
  case RVTrim::PseudoLA_TLSDESC:
    return expandLoadTLSDescAddress(MBB, MBBI);
  }

  return false;
}

bool RVTrimExpandPseudoPreRAImpl::expandAuipcInstPair(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MBBI, unsigned FlagsHi,
    unsigned SecondOpcode) const {
  MachineFunction *MF = MBB.getParent();
  MachineInstr &MI = *MBBI;
  DebugLoc DL = MI.getDebugLoc();

  Register DestReg = MI.getOperand(0).getReg();
  Register ScratchReg =
      MF->getRegInfo().createVirtualRegister(&RVTrim::GPRRegClass);

  MachineOperand &Symbol = MI.getOperand(1);
  Symbol.setTargetFlags(FlagsHi);
  MCSymbol *AUIPCSymbol = MF->getContext().createNamedTempSymbol("pcrel_hi");

  MachineInstr *MIAUIPC =
      BuildMI(MBB, MBBI, DL, TII->get(RVTrim::AUIPC), ScratchReg).add(Symbol);
  MIAUIPC->setPreInstrSymbol(*MF, AUIPCSymbol);

  MachineInstr *SecondMI =
      BuildMI(MBB, MBBI, DL, TII->get(SecondOpcode), DestReg)
          .addReg(ScratchReg)
          .addSym(AUIPCSymbol, RVTrimII::MO_PCREL_LO);

  if (MI.hasOneMemOperand())
    SecondMI->addMemOperand(*MF, *MI.memoperands_begin());

  MI.eraseFromParent();
  return true;
}

bool RVTrimExpandPseudoPreRAImpl::expandLoadLocalAddress(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MBBI) const {
  return expandAuipcInstPair(MBB, MBBI, RVTrimII::MO_PCREL_HI, RVTrim::ADDI);
}

bool RVTrimExpandPseudoPreRAImpl::expandLoadGlobalAddress(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MBBI) const {
  unsigned SecondOpcode = STI->is64Bit() ? RVTrim::LD : RVTrim::LW;
  return expandAuipcInstPair(MBB, MBBI, RVTrimII::MO_GOT_HI, SecondOpcode);
}

bool RVTrimExpandPseudoPreRAImpl::expandLoadTLSIEAddress(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MBBI) const {
  unsigned SecondOpcode = STI->is64Bit() ? RVTrim::LD : RVTrim::LW;
  return expandAuipcInstPair(MBB, MBBI, RVTrimII::MO_TLS_GOT_HI, SecondOpcode);
}

bool RVTrimExpandPseudoPreRAImpl::expandLoadTLSGDAddress(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MBBI) const {
  return expandAuipcInstPair(MBB, MBBI, RVTrimII::MO_TLS_GD_HI, RVTrim::ADDI);
}

bool RVTrimExpandPseudoPreRAImpl::expandLoadTLSDescAddress(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MBBI) const {
  MachineFunction *MF = MBB.getParent();
  MachineInstr &MI = *MBBI;
  DebugLoc DL = MI.getDebugLoc();

  const auto &STI = MF->getSubtarget<RVTrimSubtarget>();
  unsigned SecondOpcode = STI.is64Bit() ? RVTrim::LD : RVTrim::LW;

  Register FinalReg = MI.getOperand(0).getReg();
  Register DestReg =
      MF->getRegInfo().createVirtualRegister(&RVTrim::GPRRegClass);
  Register ScratchReg =
      MF->getRegInfo().createVirtualRegister(&RVTrim::GPRRegClass);

  MachineOperand &Symbol = MI.getOperand(1);
  Symbol.setTargetFlags(RVTrimII::MO_TLSDESC_HI);
  MCSymbol *AUIPCSymbol = MF->getContext().createNamedTempSymbol("tlsdesc_hi");

  MachineInstr *MIAUIPC =
      BuildMI(MBB, MBBI, DL, TII->get(RVTrim::AUIPC), ScratchReg).add(Symbol);
  MIAUIPC->setPreInstrSymbol(*MF, AUIPCSymbol);

  BuildMI(MBB, MBBI, DL, TII->get(SecondOpcode), DestReg)
      .addReg(ScratchReg)
      .addSym(AUIPCSymbol, RVTrimII::MO_TLSDESC_LOAD_LO);

  BuildMI(MBB, MBBI, DL, TII->get(RVTrim::ADDI), RVTrim::X10)
      .addReg(ScratchReg)
      .addSym(AUIPCSymbol, RVTrimII::MO_TLSDESC_ADD_LO);

  BuildMI(MBB, MBBI, DL, TII->get(RVTrim::PseudoTLSDESCCall), RVTrim::X5)
      .addReg(DestReg)
      .addImm(0)
      .addSym(AUIPCSymbol, RVTrimII::MO_TLSDESC_CALL)
      ->setPhysRegsDeadExcept(Register(RVTrim::X10), *STI.getRegisterInfo());

  BuildMI(MBB, MBBI, DL, TII->get(RVTrim::ADD), FinalReg)
      .addReg(RVTrim::X10)
      .addReg(RVTrim::X4);

  MI.eraseFromParent();
  return true;
}

char RVTrimExpandPseudoPreRALegacy::ID = 0;

INITIALIZE_PASS(RVTrimExpandPseudoPreRALegacy, "rvtrim-expand-pseudo-pre-ra",
                RVTrim_EXPAND_PSEUDO_PRE_RA_NAME, false, false)

FunctionPass *llvm::createRVTrimExpandPseudoPreRALegacyPass() {
  return new RVTrimExpandPseudoPreRALegacy();
}

PreservedAnalyses
RVTrimExpandPseudoPreRAPass::run(MachineFunction &MF,
                                MachineFunctionAnalysisManager &MFAM) {
  MFPropsModifier _(*this, MF);
  bool Changed = RVTrimExpandPseudoPreRAImpl().run(MF);
  if (!Changed)
    return PreservedAnalyses::all();

  PreservedAnalyses PA = getMachineFunctionPassPreservedAnalyses();
  PA.preserveSet<CFGAnalyses>();
  return PA;
}
