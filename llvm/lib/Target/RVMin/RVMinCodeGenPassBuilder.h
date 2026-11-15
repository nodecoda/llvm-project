//===-- RVMinCodeGenPassBuilder.h - RVMin codegen pipeline ------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_RVMIN_RVMINCODEGENPASSBUILDER_H
#define LLVM_LIB_TARGET_RVMIN_RVMINCODEGENPASSBUILDER_H

#include "llvm/IR/PassManager.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/raw_ostream.h"

namespace llvm {

class RVMinTargetMachine;
class MCContext;
class PassInstrumentationCallbacks;
struct CGPassBuilderOption;

Error buildRVMinCodeGenPipeline(RVMinTargetMachine &TM, ModulePassManager &MPM,
                                ModuleAnalysisManager &MAM,
                                raw_pwrite_stream &Out,
                                raw_pwrite_stream *DwoOut,
                                CodeGenFileType FileType,
                                const CGPassBuilderOption &Opt, MCContext &Ctx,
                                PassInstrumentationCallbacks *PIC);

} // namespace llvm

#endif // LLVM_LIB_TARGET_RVMIN_RVMINCODEGENPASSBUILDER_H
