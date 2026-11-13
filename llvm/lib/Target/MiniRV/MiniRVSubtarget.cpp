//===-- MiniRVSubtarget.cpp - MiniRV Subtarget Information ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "MiniRVSubtarget.h"
#include "MiniRVSelectionDAGInfo.h"
#include "MiniRVTargetMachine.h"

using namespace llvm;

#define DEBUG_TYPE "minirv-subtarget"

#define GET_SUBTARGETINFO_TARGET_DESC
#define GET_SUBTARGETINFO_CTOR
#include "MiniRVGenSubtargetInfo.inc"

void MiniRVSubtarget::anchor() {}

MiniRVSubtarget::MiniRVSubtarget(const Triple &TT, StringRef CPU, StringRef FS,
                                 const MiniRVTargetMachine &TM)
    : MiniRVGenSubtargetInfo(TT, CPU, /*TuneCPU=*/CPU, FS), InstrInfo(*this),
      TLInfo(TM, *this), FrameLowering(*this) {
  TSInfo = std::make_unique<MiniRVSelectionDAGInfo>();
}

MiniRVSubtarget::~MiniRVSubtarget() = default;
