//===-- GPUMinAsmPrinter.h - GPUMin assembly printer -----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_GPUMIN_GPUMINASMPRINTER_H
#define LLVM_LIB_TARGET_GPUMIN_GPUMINASMPRINTER_H

#include "llvm/CodeGen/MachineFunctionAnalysisManager.h"
#include "llvm/IR/Analysis.h"
#include "llvm/IR/PassManager.h"

namespace llvm {

class GPUMinAsmPrinterBeginPass
    : public RequiredPassInfoMixin<GPUMinAsmPrinterBeginPass> {
public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM);
};

class GPUMinAsmPrinterPass
    : public RequiredPassInfoMixin<GPUMinAsmPrinterPass> {
public:
  PreservedAnalyses run(MachineFunction &MF,
                        MachineFunctionAnalysisManager &MFAM);
};

class GPUMinAsmPrinterEndPass
    : public RequiredPassInfoMixin<GPUMinAsmPrinterEndPass> {
public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM);
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_GPUMIN_GPUMINASMPRINTER_H
