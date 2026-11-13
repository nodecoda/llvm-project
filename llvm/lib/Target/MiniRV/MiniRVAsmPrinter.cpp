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
#include "llvm/CodeGen/MachineFunctionAnalysisManager.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachinePassManager.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCStreamer.h"
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

  static char ID;
};

} // end anonymous namespace

char MiniRVAsmPrinter::ID = 0;

bool MiniRVAsmPrinter::runOnMachineFunction(MachineFunction &MF) {
  SetupMachineFunction(MF);
  emitFunctionBody();
  return false;
}

void MiniRVAsmPrinter::emitInstruction(const MachineInstr *MI) {
  // The corpus has no return instruction: PseudoRET only marks the end of a
  // function.  Print it for readability, but encode nothing into objects.
  if (MI->getOpcode() == MiniRV::PseudoRET) {
    if (OutStreamer->hasRawTextSupport())
      OutStreamer->emitRawText("\tret");
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
