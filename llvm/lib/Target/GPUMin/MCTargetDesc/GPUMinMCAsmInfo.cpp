//===-- GPUMinMCAsmInfo.cpp - GPUMin asm properties -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// GM32 is a 32-bit little-endian machine with byte-addressable memory and
// 4-byte pointers.  The syntax is LLVM's GNU-like syntax with `#` comments,
// so a GPUMin listing is readable next to the CUDA and PTX listings the
// tutorial compares against in appendix A.
//
//===----------------------------------------------------------------------===//

#include "GPUMinMCAsmInfo.h"
#include "llvm/TargetParser/Triple.h"

using namespace llvm;

void GPUMinMCAsmInfo::anchor() {}

GPUMinMCAsmInfo::GPUMinMCAsmInfo(const Triple &TT,
                                 const MCTargetOptions &Options)
    : MCAsmInfoELF(Options) {
  IsLittleEndian = true;
  CodePointerSize = 4;
  CalleeSaveStackSlotSize = 4;
  CommentString = "#";
  AlignmentIsInBytes = false;
  SupportsDebugInformation = true;
  Data16bitsDirective = "\t.half\t";
  Data32bitsDirective = "\t.word\t";
}
