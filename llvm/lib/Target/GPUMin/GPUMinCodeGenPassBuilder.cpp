//===-- GPUMinCodeGenPassBuilder.cpp - GPUMin codegen pipeline ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The new-PM codegen pipeline.  GM32 uses SelectionDAG, so the only target
// specific hook that matters in chapter 00 is the instruction selector; the
// rest of the pipeline is inherited from CodeGenPassBuilder.
//
//===----------------------------------------------------------------------===//

#include "GPUMinCodeGenPassBuilder.h"
#include "GPUMinAsmPrinter.h"
#include "GPUMinISelDAGToDAG.h"
#include "GPUMinTargetMachine.h"
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

class GPUMinCodeGenPassBuilder : public CodeGenPassBuilder {
  using Base = CodeGenPassBuilder;

  GPUMinTargetMachine &getTM() const {
    return static_cast<GPUMinTargetMachine &>(TM);
  }

public:
  explicit GPUMinCodeGenPassBuilder(GPUMinTargetMachine &TM,
                                    const CGPassBuilderOption &Opts,
                                    PassInstrumentationCallbacks *PIC)
      : CodeGenPassBuilder(TM, Opts, PIC) {}

  Error addInstSelector(PassManagerWrapper &PMW) override;
  void addAsmPrinterBegin(PassManagerWrapper &PMW) override;
  void addAsmPrinter(PassManagerWrapper &PMW) override;
  void addAsmPrinterEnd(PassManagerWrapper &PMW) override;
};

Error GPUMinCodeGenPassBuilder::addInstSelector(PassManagerWrapper &PMW) {
  addMachineFunctionPass(GPUMinDAGToDAGISelPass(getTM()), PMW);
  return Error::success();
}

void GPUMinCodeGenPassBuilder::addAsmPrinterBegin(PassManagerWrapper &PMW) {
  addModulePass(GPUMinAsmPrinterBeginPass(), PMW, /*Force=*/true);
}

void GPUMinCodeGenPassBuilder::addAsmPrinter(PassManagerWrapper &PMW) {
  addMachineFunctionPass(GPUMinAsmPrinterPass(), PMW);
}

void GPUMinCodeGenPassBuilder::addAsmPrinterEnd(PassManagerWrapper &PMW) {
  addModulePass(GPUMinAsmPrinterEndPass(), PMW, /*Force=*/true);
}

} // namespace

Error llvm::buildGPUMinCodeGenPipeline(
    GPUMinTargetMachine &TM, ModulePassManager &MPM, ModuleAnalysisManager &MAM,
    raw_pwrite_stream &Out, raw_pwrite_stream *DwoOut,
    CodeGenFileType FileType, const CGPassBuilderOption &Opt, MCContext &Ctx,
    PassInstrumentationCallbacks *PIC) {
  auto CGPB = GPUMinCodeGenPassBuilder(TM, Opt, PIC);
  return CGPB.buildPipeline(MPM, MAM, Out, DwoOut, FileType, Ctx);
}
