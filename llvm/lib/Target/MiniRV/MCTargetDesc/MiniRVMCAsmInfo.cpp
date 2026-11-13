//===-- MiniRVMCAsmInfo.cpp - MiniRV asm properties -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "MiniRVMCAsmInfo.h"
using namespace llvm;

void MiniRVMCAsmInfo::anchor() {}

MiniRVMCAsmInfo::MiniRVMCAsmInfo(const Triple &TT,
                                 const MCTargetOptions &Options)
    : MCAsmInfoELF(Options) {
  CodePointerSize = 8;
  CalleeSaveStackSlotSize = 8;

  CommentString = "#";
  AlignmentIsInBytes = false;
  UsesELFSectionDirectiveForBSS = true;
  SupportsDebugInformation = true;
}
