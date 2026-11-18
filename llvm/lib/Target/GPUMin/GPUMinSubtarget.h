//===-- GPUMinSubtarget.h - GPUMin subtarget information ------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_GPUMIN_GPUMINSUBTARGET_H
#define LLVM_LIB_TARGET_GPUMIN_GPUMINSUBTARGET_H

#include "GPUMinFrameLowering.h"
#include "GPUMinISelLowering.h"
#include "GPUMinInstrInfo.h"
#include "GPUMinRegisterInfo.h"
#include "GPUMinSelectionDAGInfo.h"
#include "MCTargetDesc/GPUMinMCTargetDesc.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include <string>

// The `GPUMin::Feature...` enum lives in MCTargetDesc/GPUMinMCTargetDesc.h
// (GET_SUBTARGETINFO_ENUM); this include only adds the class body.
#define GET_SUBTARGETINFO_HEADER
#include "GPUMinGenSubtargetInfo.inc"

namespace llvm {

class GPUMinTargetMachine;
class StringRef;

/// The per-function subtarget.  Chapter 00 has a single, featureless subtarget:
/// GM32 is one ISA with no optional extensions, so every function sees the same
/// InstrInfo/TargetLowering/FrameLowering.  Chapters 11-19 add SIMT features
/// (lanes per warp, warps per block, shared memory size) and revisit this.
class GPUMinSubtarget : public GPUMinGenSubtargetInfo {
  virtual void anchor();

  GPUMinInstrInfo InstrInfo;
  GPUMinTargetLowering TLInfo;
  GPUMinFrameLowering FrameLowering;
  std::unique_ptr<const SelectionDAGTargetInfo> TSInfo;

public:
  GPUMinSubtarget(const Triple &TT, const std::string &CPU,
                  const std::string &FS, const GPUMinTargetMachine &TM);

  GPUMinSubtarget &initializeSubtargetDependencies(StringRef CPU, StringRef FS);

  void ParseSubtargetFeatures(StringRef CPU, StringRef TuneCPU, StringRef FS);

  const GPUMinInstrInfo *getInstrInfo() const override { return &InstrInfo; }
  const GPUMinRegisterInfo *getRegisterInfo() const override {
    return &InstrInfo.getRegisterInfo();
  }
  const GPUMinTargetLowering *getTargetLowering() const override {
    return &TLInfo;
  }
  const TargetFrameLowering *getFrameLowering() const override {
    return &FrameLowering;
  }
  const SelectionDAGTargetInfo *getSelectionDAGInfo() const override {
    return TSInfo.get();
  }
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_GPUMIN_GPUMINSUBTARGET_H
