//===-- RVMinAsmPrinter.cpp - RVMin assembly printer ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// AsmPrinter walks the MachineFunction and hands each instruction to the MC
// layer.  MachineInstr -> MCInst conversion lives in RVMinMCInstLower.cpp.
//
//===----------------------------------------------------------------------===//

#include "RVMinAsmPrinter.h"
#include "RVMin.h"
#include "RVMinMCInstLower.h"
#include "RVMinTargetMachine.h"
#include "TargetInfo/RVMinTargetInfo.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/CodeGen/AsmPrinterAnalysis.h"
#include "llvm/CodeGen/MachineFunctionAnalysisManager.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachinePassManager.h"
#include "llvm/IR/Analysis.h"
#include "llvm/IR/PassManager.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

namespace {

class RVMinAsmPrinter : public AsmPrinter {
public:
  RVMinAsmPrinter(TargetMachine &TM, std::unique_ptr<MCStreamer> Streamer)
      : AsmPrinter(TM, std::move(Streamer), ID) {}

  StringRef getPassName() const override { return "RVMin Assembly Printer"; }

  bool runOnMachineFunction(MachineFunction &MF) override {
    SetupMachineFunction(MF);
    emitFunctionBody();
    return false;
  }

  void emitInstruction(const MachineInstr *MI) override {
    if (MI->isPseudo()) {
      MCInst Expanded;
      if (lowerPseudoInstExpansion(MI, Expanded)) {
        OutStreamer->emitInstruction(Expanded, getSubtargetInfo());
        return;
      }
      llvm_unreachable("RVMin: unimplemented pseudo instruction reached the "
                       "asm printer");
    }

    MCInst TmpInst;
    LowerRVMinMachineInstrToMCInst(MI, TmpInst, OutContext);
    OutStreamer->emitInstruction(TmpInst, getSubtargetInfo());
  }

  /// TableGen-generated expansion for instructions declared with
  /// PseudoInstExpansion (PseudoRET and PseudoBR).
  bool lowerPseudoInstExpansion(const MachineInstr *MI, MCInst &Inst);

  /// Operand helper used by the generated pseudo-expansion code: a pseudo
  /// branch carries a basic block, which becomes a symbol reference.
  void lowerOperand(const MachineOperand &MO, MCOperand &MCOp) {
    switch (MO.getType()) {
    case MachineOperand::MO_Register:
      MCOp = MCOperand::createReg(MO.getReg());
      return;
    case MachineOperand::MO_Immediate:
      MCOp = MCOperand::createImm(MO.getImm());
      return;
    case MachineOperand::MO_MachineBasicBlock:
      MCOp = MCOperand::createExpr(
          MCSymbolRefExpr::create(MO.getMBB()->getSymbol(), OutContext));
      return;
    default:
      llvm_unreachable("RVMin: unsupported pseudo-instruction operand");
    }
  }

  static char ID;
};

char RVMinAsmPrinter::ID = 0;

#include "RVMinGenMCPseudoLowering.inc"

} // namespace

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializeRVMinAsmPrinter() {
  RegisterAsmPrinter<RVMinAsmPrinter> X(getTheRVMinTarget());
}

PreservedAnalyses RVMinAsmPrinterBeginPass::run(Module &M,
                                                ModuleAnalysisManager &MAM) {
  RVMinAsmPrinter &AsmPrinter = static_cast<RVMinAsmPrinter &>(
      MAM.getResult<AsmPrinterAnalysis>(M).getPrinter());
  setupModuleAsmPrinter(M, MAM, AsmPrinter);
  AsmPrinter.doInitialization(M);
  return PreservedAnalyses::all();
}

PreservedAnalyses
RVMinAsmPrinterPass::run(MachineFunction &MF,
                         MachineFunctionAnalysisManager &MFAM) {
  RVMinAsmPrinter &AsmPrinter = static_cast<RVMinAsmPrinter &>(
      MFAM.getResult<ModuleAnalysisManagerMachineFunctionProxy>(MF)
          .getCachedResult<AsmPrinterAnalysis>(*MF.getFunction().getParent())
          ->getPrinter());
  setupMachineFunctionAsmPrinter(MFAM, MF, AsmPrinter);
  AsmPrinter.runOnMachineFunction(MF);
  return PreservedAnalyses::all();
}

PreservedAnalyses RVMinAsmPrinterEndPass::run(Module &M,
                                              ModuleAnalysisManager &MAM) {
  RVMinAsmPrinter &AsmPrinter = static_cast<RVMinAsmPrinter &>(
      MAM.getResult<AsmPrinterAnalysis>(M).getPrinter());
  setupModuleAsmPrinter(M, MAM, AsmPrinter);
  AsmPrinter.doFinalization(M);
  return PreservedAnalyses::all();
}
