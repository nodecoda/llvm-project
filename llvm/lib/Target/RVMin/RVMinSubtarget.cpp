//===-- RVMinSubtarget.cpp - RVMin subtarget information -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "RVMinSubtarget.h"
#include "RVMinTargetMachine.h"

using namespace llvm;

#define DEBUG_TYPE "rvmin-subtarget"

#define GET_SUBTARGETINFO_TARGET_DESC
#define GET_SUBTARGETINFO_CTOR
#include "RVMinGenSubtargetInfo.inc"

void RVMinSubtarget::anchor() {}

RVMinSubtarget &
RVMinSubtarget::initializeSubtargetDependencies(StringRef CPU, StringRef FS) {
  StringRef CPUName = CPU.empty() ? "generic-rv32" : CPU;
  ParseSubtargetFeatures(CPUName, /*TuneCPU=*/CPUName, FS);
  return *this;
}

RVMinSubtarget::RVMinSubtarget(const Triple &TT, const std::string &CPU,
                               const std::string &FS,
                               const RVMinTargetMachine &TM)
    : RVMinGenSubtargetInfo(TT, CPU, /*TuneCPU=*/CPU, FS),
      InstrInfo(initializeSubtargetDependencies(CPU, FS)), TLInfo(TM, *this),
      FrameLowering(*this) {
  TSInfo = std::make_unique<RVMinSelectionDAGInfo>();
}
