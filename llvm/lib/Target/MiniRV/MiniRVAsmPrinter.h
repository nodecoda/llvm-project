//===-- MiniRVAsmPrinter.h - MiniRV LLVM assembly writer --------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_MINIRV_MINIRVASMPRINTER_H
#define LLVM_LIB_TARGET_MINIRV_MINIRVASMPRINTER_H

#include "llvm/CodeGen/MachineFunctionAnalysisManager.h"
#include "llvm/IR/Analysis.h"
#include "llvm/IR/PassManager.h"

namespace llvm {

class MiniRVAsmPrinterBeginPass
    : public RequiredPassInfoMixin<MiniRVAsmPrinterBeginPass> {
public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM);
};

class MiniRVAsmPrinterPass
    : public RequiredPassInfoMixin<MiniRVAsmPrinterPass> {
public:
  PreservedAnalyses run(MachineFunction &MF,
                        MachineFunctionAnalysisManager &MFAM);
};

class MiniRVAsmPrinterEndPass
    : public RequiredPassInfoMixin<MiniRVAsmPrinterEndPass> {
public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM);
};

} // end namespace llvm

#endif // LLVM_LIB_TARGET_MINIRV_MINIRVASMPRINTER_H
