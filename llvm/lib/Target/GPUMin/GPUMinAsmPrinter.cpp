//===-- GPUMinAsmPrinter.cpp - GPUMin assembly printer --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// AsmPrinter walks the MachineFunction and hands each instruction to the MC
// layer.  MachineInstr -> MCInst conversion lives in GPUMinMCInstLower.cpp.
//
//===----------------------------------------------------------------------===//

#include "GPUMinAsmPrinter.h"
#include "GPUMinMCInstLower.h"
#include "GPUMinTargetMachine.h"
#include "MCTargetDesc/GPUMinInstPrinter.h"
#include "TargetInfo/GPUMinTargetInfo.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/CodeGen/AsmPrinterAnalysis.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunctionAnalysisManager.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachinePassManager.h"
#include "llvm/IR/Analysis.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/PassManager.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

namespace {

class GPUMinAsmPrinter : public AsmPrinter {
public:
  GPUMinAsmPrinter(TargetMachine &TM, std::unique_ptr<MCStreamer> Streamer)
      : AsmPrinter(TM, std::move(Streamer), ID) {}

  StringRef getPassName() const override { return "GPUMin Assembly Printer"; }

  void emitInstruction(const MachineInstr *MI) override {
    // PseudoRET expands into the real EXIT word through the TableGen
    // pseudo-lowering table (PseudoInstExpansion in GPUMinInstrInfo.td).
    if (MI->isPseudo()) {
      MCInst Expanded;
      if (lowerPseudoInstExpansion(MI, Expanded)) {
        EmitToStreamer(*OutStreamer, Expanded);
        return;
      }
      llvm_unreachable("GPUMin: unimplemented pseudo instruction reached the "
                       "asm printer");
    }

    MCInst TmpInst;
    LowerGPUMinMachineInstrToMCInst(MI, TmpInst, OutContext, TM, *this);
    EmitToStreamer(*OutStreamer, TmpInst);
  }

  /// TableGen-generated expansion for instructions declared with
  /// PseudoInstExpansion (PseudoRET in chapter 00).
  bool lowerPseudoInstExpansion(const MachineInstr *MI, MCInst &Inst);

  /// Chapter 16: address space 2 is the block's shared window.  The launch
  /// runtime allocates it, so it is not part of the object image and must not
  /// become a `.bss` definition plus a symbol.
  void emitGlobalVariable(const GlobalVariable *GV) override {
    if (GV->getAddressSpace() == 2)
      return;
    AsmPrinter::emitGlobalVariable(GV);
  }

  static char ID;
};

char GPUMinAsmPrinter::ID = 0;

#include "GPUMinGenMCPseudoLowering.inc"

} // namespace

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializeGPUMinAsmPrinter() {
  RegisterAsmPrinter<GPUMinAsmPrinter> X(getTheGPUMinTarget());
}

PreservedAnalyses GPUMinAsmPrinterBeginPass::run(Module &M,
                                                 ModuleAnalysisManager &MAM) {
  GPUMinAsmPrinter &AsmPrinter = static_cast<GPUMinAsmPrinter &>(
      MAM.getResult<AsmPrinterAnalysis>(M).getPrinter());
  setupModuleAsmPrinter(M, MAM, AsmPrinter);
  AsmPrinter.doInitialization(M);
  return PreservedAnalyses::all();
}

PreservedAnalyses
GPUMinAsmPrinterPass::run(MachineFunction &MF,
                          MachineFunctionAnalysisManager &MFAM) {
  GPUMinAsmPrinter &AsmPrinter = static_cast<GPUMinAsmPrinter &>(
      MFAM.getResult<ModuleAnalysisManagerMachineFunctionProxy>(MF)
          .getCachedResult<AsmPrinterAnalysis>(*MF.getFunction().getParent())
          ->getPrinter());
  setupMachineFunctionAsmPrinter(MFAM, MF, AsmPrinter);
  AsmPrinter.runOnMachineFunction(MF);
  return PreservedAnalyses::all();
}

PreservedAnalyses GPUMinAsmPrinterEndPass::run(Module &M,
                                               ModuleAnalysisManager &MAM) {
  GPUMinAsmPrinter &AsmPrinter = static_cast<GPUMinAsmPrinter &>(
      MAM.getResult<AsmPrinterAnalysis>(M).getPrinter());
  setupModuleAsmPrinter(M, MAM, AsmPrinter);
  AsmPrinter.doFinalization(M);
  return PreservedAnalyses::all();
}
