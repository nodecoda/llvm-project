//===-- GPUMinSubtarget.cpp - GPUMin subtarget information ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "GPUMinSubtarget.h"
#include "GPUMinTargetMachine.h"

using namespace llvm;

#define DEBUG_TYPE "gpumin-subtarget"

#define GET_SUBTARGETINFO_TARGET_DESC
#define GET_SUBTARGETINFO_CTOR
#include "GPUMinGenSubtargetInfo.inc"

void GPUMinSubtarget::anchor() {}

GPUMinSubtarget &
GPUMinSubtarget::initializeSubtargetDependencies(StringRef CPU, StringRef FS) {
  StringRef CPUName = CPU.empty() ? "generic" : CPU;
  ParseSubtargetFeatures(CPUName, /*TuneCPU=*/CPUName, FS);
  return *this;
}

GPUMinSubtarget::GPUMinSubtarget(const Triple &TT, const std::string &CPU,
                                 const std::string &FS,
                                 const GPUMinTargetMachine &TM)
    : GPUMinGenSubtargetInfo(TT, CPU, /*TuneCPU=*/CPU, FS),
      InstrInfo(initializeSubtargetDependencies(CPU, FS)), TLInfo(TM, *this),
      FrameLowering(*this) {
  TSInfo = std::make_unique<GPUMinSelectionDAGInfo>();
}
