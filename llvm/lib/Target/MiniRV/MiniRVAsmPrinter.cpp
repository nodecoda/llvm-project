//===-- MiniRVAsmPrinter.cpp - MiniRV LLVM assembly writer ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "MiniRVAsmPrinter.h"
#include "MiniRVMCInstLower.h"
#include "MiniRVTargetMachine.h"
#include "TargetInfo/MiniRVTargetInfo.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/CodeGen/AsmPrinterAnalysis.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunctionAnalysisManager.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachinePassManager.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Compiler.h"

using namespace llvm;

namespace {

class MiniRVAsmPrinter : public AsmPrinter {
public:
  MiniRVAsmPrinter(TargetMachine &TM, std::unique_ptr<MCStreamer> Streamer)
      : AsmPrinter(TM, std::move(Streamer), ID) {}

  StringRef getPassName() const override { return "MiniRV Assembly Printer"; }

  bool runOnMachineFunction(MachineFunction &MF) override;
  void emitInstruction(const MachineInstr *MI) override;

  // Wrapper needed for the tablegenned pseudo lowering.
  bool lowerOperand(const MachineOperand &MO, MCOperand &MCOp) const;
  bool lowerPseudoInstExpansion(const MachineInstr *MI, MCInst &Inst);

  void emitSymbolAddress(const MachineInstr &MI);

  static char ID;
};

} // end anonymous namespace

char MiniRVAsmPrinter::ID = 0;

bool MiniRVAsmPrinter::runOnMachineFunction(MachineFunction &MF) {
  SetupMachineFunction(MF);
  emitFunctionBody();
  return false;
}

bool MiniRVAsmPrinter::lowerOperand(const MachineOperand &MO,
                                    MCOperand &MCOp) const {
  switch (MO.getType()) {
  default:
    return false;
  case MachineOperand::MO_Register:
    if (MO.isImplicit())
      return false;
    MCOp = MCOperand::createReg(MO.getReg());
    return true;
  case MachineOperand::MO_Immediate:
    MCOp = MCOperand::createImm(MO.getImm());
    return true;
  case MachineOperand::MO_MachineBasicBlock:
    MCOp = MCOperand::createExpr(
        MCSymbolRefExpr::create(MO.getMBB()->getSymbol(), OutContext));
    return true;
  case MachineOperand::MO_GlobalAddress:
    MCOp = MCOperand::createExpr(
        MCSymbolRefExpr::create(getSymbol(MO.getGlobal()), OutContext));
    return true;
  case MachineOperand::MO_ExternalSymbol:
    MCOp = MCOperand::createExpr(MCSymbolRefExpr::create(
        GetExternalSymbolSymbol(MO.getSymbolName()), OutContext));
    return true;
  case MachineOperand::MO_MCSymbol:
    MCOp = MCOperand::createExpr(
        MCSymbolRefExpr::create(MO.getMCSymbol(), OutContext));
    return true;
  }
}

/// Emit "auipc rd, %pcrel_hi(sym)" followed by "addi rd, rd, %pcrel_lo(L)".
/// The label L marks the auipc so that the assembler and the linker can find
/// the high part that the low part belongs to.
void MiniRVAsmPrinter::emitSymbolAddress(const MachineInstr &MI) {
  Register Dst = MI.getOperand(0).getReg();
  const MachineOperand &MO = MI.getOperand(1);

  const MCExpr *Target;
  if (MO.isGlobal())
    Target = MCSymbolRefExpr::create(getSymbol(MO.getGlobal()), MO.getOffset(),
                                     OutContext);
  else
    Target = MCSymbolRefExpr::create(
        GetExternalSymbolSymbol(MO.getSymbolName()), OutContext);

  MCSymbol *HiLabel = OutContext.createTempSymbol("pcrel_hi", true);
  OutStreamer->emitLabel(HiLabel);

  MCInst AUIPC;
  AUIPC.setOpcode(MiniRV::AUIPC);
  AUIPC.addOperand(MCOperand::createReg(Dst));
  AUIPC.addOperand(MCOperand::createExpr(Target));
  EmitToStreamer(*OutStreamer, AUIPC);

  MCInst ADDI;
  ADDI.setOpcode(MiniRV::ADDI);
  ADDI.addOperand(MCOperand::createReg(Dst));
  ADDI.addOperand(MCOperand::createReg(Dst));
  ADDI.addOperand(
      MCOperand::createExpr(MCSymbolRefExpr::create(HiLabel, OutContext)));
  EmitToStreamer(*OutStreamer, ADDI);
}

// PseudoRET and PseudoBR are lowered here, from the records that carry a
// PseudoInstExpansion in MiniRVInstrInfo.td.
#include "MiniRVGenMCPseudoLowering.inc"

void MiniRVAsmPrinter::emitInstruction(const MachineInstr *MI) {
  if (MI->getOpcode() == MiniRV::PseudoLA) {
    emitSymbolAddress(*MI);
    return;
  }

  if (MCInst Expanded; lowerPseudoInstExpansion(MI, Expanded)) {
    EmitToStreamer(*OutStreamer, Expanded);
    return;
  }

  MiniRVMCInstLower MCInstLowering(OutContext, *this);
  MCInst TmpInst;
  MCInstLowering.Lower(MI, TmpInst);
  EmitToStreamer(*OutStreamer, TmpInst);
}

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializeMiniRVAsmPrinter() {
  RegisterAsmPrinter<MiniRVAsmPrinter> X(getTheMiniRVTarget());
}

PreservedAnalyses MiniRVAsmPrinterBeginPass::run(Module &M,
                                                 ModuleAnalysisManager &MAM) {
  MiniRVAsmPrinter &AsmPrinter = static_cast<MiniRVAsmPrinter &>(
      MAM.getResult<AsmPrinterAnalysis>(M).getPrinter());
  setupModuleAsmPrinter(M, MAM, AsmPrinter);
  AsmPrinter.doInitialization(M);
  return PreservedAnalyses::all();
}

PreservedAnalyses
MiniRVAsmPrinterPass::run(MachineFunction &MF,
                          MachineFunctionAnalysisManager &MFAM) {
  MiniRVAsmPrinter &AsmPrinter = static_cast<MiniRVAsmPrinter &>(
      MFAM.getResult<ModuleAnalysisManagerMachineFunctionProxy>(MF)
          .getCachedResult<AsmPrinterAnalysis>(*MF.getFunction().getParent())
          ->getPrinter());
  setupMachineFunctionAsmPrinter(MFAM, MF, AsmPrinter);
  AsmPrinter.runOnMachineFunction(MF);
  return PreservedAnalyses::all();
}

PreservedAnalyses MiniRVAsmPrinterEndPass::run(Module &M,
                                               ModuleAnalysisManager &MAM) {
  MiniRVAsmPrinter &AsmPrinter = static_cast<MiniRVAsmPrinter &>(
      MAM.getResult<AsmPrinterAnalysis>(M).getPrinter());
  setupModuleAsmPrinter(M, MAM, AsmPrinter);
  AsmPrinter.doFinalization(M);
  return PreservedAnalyses::all();
}
