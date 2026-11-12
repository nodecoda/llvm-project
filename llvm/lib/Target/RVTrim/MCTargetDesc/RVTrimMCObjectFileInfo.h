//===-- RVTrimMCObjectFileInfo.h - RISC-V object file Info ------*- C++ -*--===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains the declaration of the RVTrimMCObjectFileInfo class.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_RVTrim_MCTARGETDESC_RVTrimMCOBJECTFILEINFO_H
#define LLVM_LIB_TARGET_RVTrim_MCTARGETDESC_RVTrimMCOBJECTFILEINFO_H

#include "llvm/MC/MCObjectFileInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"

namespace llvm {

class RVTrimMCObjectFileInfo : public MCObjectFileInfo {
public:
  static unsigned getTextSectionAlignment(const MCSubtargetInfo &STI);
  unsigned getTextSectionAlignment() const override;
};

} // namespace llvm

#endif
