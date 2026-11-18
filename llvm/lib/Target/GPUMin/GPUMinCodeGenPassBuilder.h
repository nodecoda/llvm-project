//===-- GPUMinCodeGenPassBuilder.h - GPUMin codegen pipeline ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_GPUMIN_GPUMINCODEGENPASSBUILDER_H
#define LLVM_LIB_TARGET_GPUMIN_GPUMINCODEGENPASSBUILDER_H

#include "llvm/IR/PassManager.h"
#include "llvm/Support/CodeGen.h"

namespace llvm {

class GPUMinTargetMachine;
class MCContext;
class PassInstrumentationCallbacks;
class raw_pwrite_stream;
struct CGPassBuilderOption;

Error buildGPUMinCodeGenPipeline(GPUMinTargetMachine &TM,
                                 ModulePassManager &MPM,
                                 ModuleAnalysisManager &MAM,
                                 raw_pwrite_stream &Out,
                                 raw_pwrite_stream *DwoOut,
                                 CodeGenFileType FileType,
                                 const CGPassBuilderOption &Opt,
                                 MCContext &Ctx,
                                 PassInstrumentationCallbacks *PIC);

} // namespace llvm

#endif // LLVM_LIB_TARGET_GPUMIN_GPUMINCODEGENPASSBUILDER_H
