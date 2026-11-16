//===-- RVMinSubtarget.h - RVMin subtarget information ----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_RVMIN_RVMINSUBTARGET_H
#define LLVM_LIB_TARGET_RVMIN_RVMINSUBTARGET_H

#include "MCTargetDesc/RVMinMCTargetDesc.h"
#include "RVMinFrameLowering.h"
#include "RVMinISelLowering.h"
#include "RVMinInstrInfo.h"
#include "RVMinRegisterInfo.h"
#include "RVMinSelectionDAGInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include <string>

// The `RVMin::Feature...` enum lives in MCTargetDesc/RVMinMCTargetDesc.h
// (GET_SUBTARGETINFO_ENUM above); this include only adds the class body.
#define GET_SUBTARGETINFO_HEADER
#include "RVMinGenSubtargetInfo.inc"

namespace llvm {

class RVMinTargetMachine;
class StringRef;

class RVMinSubtarget : public RVMinGenSubtargetInfo {
  virtual void anchor();

  // Chapter 13: one bit per optional extension.  `ParseSubtargetFeatures`
  // (generated from RVMin.td) writes these fields when the feature string
  // contains `+m`.
  bool HasStdExtM = false;
  // Chapter 21: Zbb/Zbs are independent optional extensions, so each gets its
  // own feature bit and query.
  bool HasStdExtZbb = false;
  bool HasStdExtZbs = false;

  RVMinInstrInfo InstrInfo;
  RVMinTargetLowering TLInfo;
  RVMinFrameLowering FrameLowering;
  std::unique_ptr<const SelectionDAGTargetInfo> TSInfo;

public:
  RVMinSubtarget(const Triple &TT, const std::string &CPU,
                 const std::string &FS, const RVMinTargetMachine &TM);

  RVMinSubtarget &initializeSubtargetDependencies(StringRef CPU, StringRef FS);

  // Feature query used by TableGen predicates (`HasStdExtM` in the .td files)
  // and by hand-written lowering code.
  bool hasStdExtM() const { return HasStdExtM; }
  bool hasStdExtZbb() const { return HasStdExtZbb; }
  bool hasStdExtZbs() const { return HasStdExtZbs; }

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
