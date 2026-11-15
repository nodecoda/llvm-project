//===-- RVMinSubtarget.h - RVMin subtarget information ----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_RVMIN_RVMINSUBTARGET_H
#define LLVM_LIB_TARGET_RVMIN_RVMINSUBTARGET_H

#include "RVMinFrameLowering.h"
#include "RVMinISelLowering.h"
#include "RVMinInstrInfo.h"
#include "RVMinRegisterInfo.h"
#include "RVMinSelectionDAGInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include <string>

#define GET_SUBTARGETINFO_HEADER
#include "RVMinGenSubtargetInfo.inc"

namespace llvm {

class RVMinTargetMachine;
class StringRef;

class RVMinSubtarget : public RVMinGenSubtargetInfo {
  virtual void anchor();

  RVMinInstrInfo InstrInfo;
  RVMinTargetLowering TLInfo;
  RVMinFrameLowering FrameLowering;
  std::unique_ptr<const SelectionDAGTargetInfo> TSInfo;

public:
  RVMinSubtarget(const Triple &TT, const std::string &CPU,
                 const std::string &FS, const RVMinTargetMachine &TM);

  RVMinSubtarget &initializeSubtargetDependencies(StringRef CPU, StringRef FS);

  void ParseSubtargetFeatures(StringRef CPU, StringRef TuneCPU, StringRef FS);

  const RVMinInstrInfo *getInstrInfo() const override { return &InstrInfo; }
  const RVMinRegisterInfo *getRegisterInfo() const override {
    return &InstrInfo.getRegisterInfo();
  }
  const RVMinTargetLowering *getTargetLowering() const override {
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

#endif // LLVM_LIB_TARGET_RVMIN_RVMINSUBTARGET_H
