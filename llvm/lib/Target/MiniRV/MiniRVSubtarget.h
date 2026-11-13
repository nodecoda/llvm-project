//===-- MiniRVSubtarget.h - Define Subtarget for MiniRV ---------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_MINIRV_MINIRVSUBTARGET_H
#define LLVM_LIB_TARGET_MINIRV_MINIRVSUBTARGET_H

#include "MiniRVFrameLowering.h"
#include "MiniRVISelLowering.h"
#include "MiniRVInstrInfo.h"
#include "MiniRVRegisterInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"

#define GET_SUBTARGETINFO_HEADER
#include "MiniRVGenSubtargetInfo.inc"

namespace llvm {

class MiniRVTargetMachine;

class MiniRVSubtarget : public MiniRVGenSubtargetInfo {
  virtual void anchor();

  MiniRVInstrInfo InstrInfo;
  MiniRVTargetLowering TLInfo;
  std::unique_ptr<const SelectionDAGTargetInfo> TSInfo;
  MiniRVFrameLowering FrameLowering;

public:
  MiniRVSubtarget(const Triple &TT, StringRef CPU, StringRef FS,
                  const MiniRVTargetMachine &TM);
  ~MiniRVSubtarget() override;

  void ParseSubtargetFeatures(StringRef CPU, StringRef TuneCPU, StringRef FS);

  const TargetFrameLowering *getFrameLowering() const override {
    return &FrameLowering;
  }
  const MiniRVInstrInfo *getInstrInfo() const override { return &InstrInfo; }
  const MiniRVRegisterInfo *getRegisterInfo() const override {
    return &InstrInfo.getRegisterInfo();
  }
  const MiniRVTargetLowering *getTargetLowering() const override {
    return &TLInfo;
  }
  const SelectionDAGTargetInfo *getSelectionDAGInfo() const override {
    return TSInfo.get();
  }

  void initLibcallLoweringInfo(LibcallLoweringInfo &Info) const override;
};

} // end namespace llvm

#endif // LLVM_LIB_TARGET_MINIRV_MINIRVSUBTARGET_H
