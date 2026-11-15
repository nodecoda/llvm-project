//===-- RVMinCodeGenPassBuilder.cpp - RVMin codegen pipeline -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "RVMinCodeGenPassBuilder.h"
#include "RVMinAsmPrinter.h"
#include "RVMinTargetMachine.h"
#include "llvm/CodeGen/MachineFunctionAnalysisManager.h"
#include "llvm/IR/Analysis.h"
#include "llvm/IR/PassInstrumentation.h"
#include "llvm/IR/PassManager.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/Passes/CodeGenPassBuilder.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Target/CGPassBuilderOption.h"

using namespace llvm;

namespace {

class RVMinUnimplementedISelPass
    : public RequiredPassInfoMixin<RVMinUnimplementedISelPass> {
public:
  PreservedAnalyses run(MachineFunction &MF,
                        MachineFunctionAnalysisManager &MFAM) {
    report_fatal_error("RVMin: instruction selection is not implemented yet");
  }
};

class RVMinCodeGenPassBuilder : public CodeGenPassBuilder {
  using Base = CodeGenPassBuilder;

  RVMinTargetMachine &getTM() const {
    return static_cast<RVMinTargetMachine &>(TM);
  }

public:
  explicit RVMinCodeGenPassBuilder(RVMinTargetMachine &TM,
                                   const CGPassBuilderOption &Opts,
                                   PassInstrumentationCallbacks *PIC)
      : CodeGenPassBuilder(TM, Opts, PIC) {}

  Error addInstSelector(PassManagerWrapper &PMW) override;
  void addAsmPrinterBegin(PassManagerWrapper &PMW) override;
  void addAsmPrinter(PassManagerWrapper &PMW) override;
  void addAsmPrinterEnd(PassManagerWrapper &PMW) override;
};

Error RVMinCodeGenPassBuilder::addInstSelector(PassManagerWrapper &PMW) {
  // Chapter 0 has no instruction selector yet.  Empty modules still build a
  // complete pipeline; non-empty ones fail with a clear diagnostic instead of
  // tripping an assertion later in the machine pass pipeline.
  addMachineFunctionPass(RVMinUnimplementedISelPass(), PMW);
  return Error::success();
}

void RVMinCodeGenPassBuilder::addAsmPrinterBegin(PassManagerWrapper &PMW) {
  addModulePass(RVMinAsmPrinterBeginPass(), PMW, /*Force=*/true);
}

void RVMinCodeGenPassBuilder::addAsmPrinter(PassManagerWrapper &PMW) {
  addMachineFunctionPass(RVMinAsmPrinterPass(), PMW);
}

void RVMinCodeGenPassBuilder::addAsmPrinterEnd(PassManagerWrapper &PMW) {
  addModulePass(RVMinAsmPrinterEndPass(), PMW, /*Force=*/true);
}

} // namespace

Error llvm::buildRVMinCodeGenPipeline(
    RVMinTargetMachine &TM, ModulePassManager &MPM, ModuleAnalysisManager &MAM,
    raw_pwrite_stream &Out, raw_pwrite_stream *DwoOut,
    CodeGenFileType FileType, const CGPassBuilderOption &Opt, MCContext &Ctx,
    PassInstrumentationCallbacks *PIC) {
  auto CGPB = RVMinCodeGenPassBuilder(TM, Opt, PIC);
  return CGPB.buildPipeline(MPM, MAM, Out, DwoOut, FileType, Ctx);
}
