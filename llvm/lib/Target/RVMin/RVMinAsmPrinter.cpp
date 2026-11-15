//===-- RVMinAsmPrinter.cpp - RVMin assembly printer ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Chapter 0 only needs the AsmPrinter plumbing so that llc can build a codegen
// pipeline.  Tree walking and instruction printing arrive in chapter 2.
//
//===----------------------------------------------------------------------===//

#include "RVMinAsmPrinter.h"
#include "RVMinTargetMachine.h"
#include "TargetInfo/RVMinTargetInfo.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/CodeGen/AsmPrinterAnalysis.h"
#include "llvm/CodeGen/MachineFunctionAnalysisManager.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachinePassManager.h"
#include "llvm/IR/Analysis.h"
#include "llvm/IR/PassManager.h"
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
    llvm_unreachable("RVMin instruction emission is not implemented yet");
  }

  static char ID;
};

char RVMinAsmPrinter::ID = 0;

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
