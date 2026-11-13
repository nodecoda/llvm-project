//===-- MiniRVCodeGenPassBuilder.cpp - MiniRV codegen pass pipeline ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "MiniRV.h"
#include "MiniRVAsmPrinter.h"
#include "MiniRVTargetMachine.h"
#include "llvm/CodeGen/AtomicExpand.h"
#include "llvm/CodeGen/SelectionDAGISel.h"
#include "llvm/IR/PassInstrumentation.h"
#include "llvm/Passes/CodeGenPassBuilder.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Target/CGPassBuilderOption.h"

using namespace llvm;

namespace {

class MiniRVCodeGenPassBuilder : public CodeGenPassBuilder {
  using Base = CodeGenPassBuilder;

  MiniRVTargetMachine &getTM() const {
    return static_cast<MiniRVTargetMachine &>(TM);
  }

public:
  explicit MiniRVCodeGenPassBuilder(MiniRVTargetMachine &TM,
                                    const CGPassBuilderOption &Opts,
                                    PassInstrumentationCallbacks *PIC)
      : CodeGenPassBuilder(TM, Opts, PIC) {}

  void addIRPasses(PassManagerWrapper &PMW) override;
  Error addInstSelector(PassManagerWrapper &PMW) override;
  void addAsmPrinterBegin(PassManagerWrapper &PMW) override;
  void addAsmPrinter(PassManagerWrapper &PMW) override;
  void addAsmPrinterEnd(PassManagerWrapper &PMW) override;
};

void MiniRVCodeGenPassBuilder::addIRPasses(PassManagerWrapper &PMW) {
  // There is no A extension, so there is no legal atomic instruction.  Expand
  // every atomic load, store, exchange, compare-exchange and read-modify-write
  // into the corresponding libatomic entry point before instruction selection;
  // without this the selector sees ATOMIC_* nodes it has no pattern for.
  addFunctionPass(AtomicExpandPass(getTM()), PMW);
  Base::addIRPasses(PMW);
}

Error MiniRVCodeGenPassBuilder::addInstSelector(PassManagerWrapper &PMW) {
  addMachineFunctionPass(MiniRVISelDAGToDAGPass(getTM(), getOptLevel()), PMW);
  return Error::success();
}

void MiniRVCodeGenPassBuilder::addAsmPrinterBegin(PassManagerWrapper &PMW) {
  addModulePass(MiniRVAsmPrinterBeginPass(), PMW, /*Force=*/true);
}

void MiniRVCodeGenPassBuilder::addAsmPrinter(PassManagerWrapper &PMW) {
  addMachineFunctionPass(MiniRVAsmPrinterPass(), PMW);
}

void MiniRVCodeGenPassBuilder::addAsmPrinterEnd(PassManagerWrapper &PMW) {
  addModulePass(MiniRVAsmPrinterEndPass(), PMW);
}

} // end anonymous namespace

Error MiniRVTargetMachine::buildCodeGenPipeline(
    ModulePassManager &MPM, ModuleAnalysisManager &MAM, raw_pwrite_stream &Out,
    raw_pwrite_stream *DwoOut, CodeGenFileType FileType,
    const CGPassBuilderOption &Opt, MCContext &Ctx,
    PassInstrumentationCallbacks *PIC) {
  auto CGPB = MiniRVCodeGenPassBuilder(*this, Opt, PIC);
  return CGPB.buildPipeline(MPM, MAM, Out, DwoOut, FileType, Ctx);
}
