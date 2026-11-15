//===-- RVMinRegisterInfo.h - RVMin register information -------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_RVMIN_RVMINREGISTERINFO_H
#define LLVM_LIB_TARGET_RVMIN_RVMINREGISTERINFO_H

#include "llvm/CodeGen/TargetRegisterInfo.h"

#define GET_REGINFO_HEADER
#include "RVMinGenRegisterInfo.inc"

namespace llvm {

class RVMinRegisterInfo : public RVMinGenRegisterInfo {
public:
  RVMinRegisterInfo();

  const MCPhysReg *getCalleeSavedRegs(const MachineFunction *MF) const override;
  const uint32_t *getCallPreservedMask(const MachineFunction &MF,
                                       CallingConv::ID CC) const override;
  BitVector getReservedRegs(const MachineFunction &MF) const override;
  bool eliminateFrameIndex(MachineBasicBlock::iterator II, int SPAdj,
                           unsigned FIOperandNum,
                           RegScavenger *RS = nullptr) const override;
  Register getFrameRegister(const MachineFunction &MF) const override;

  /// Chapter 16: frames larger than 2 KiB need a scratch register after
  /// register allocation, so PEI must build a RegScavenger for every function.
  bool requiresRegisterScavenging(const MachineFunction &MF) const override {
    return true;
  }

  /// eliminateFrameIndex creates virtual registers for far frame accesses and
  /// leaves them to scavengeFrameVirtualRegs (see eliminateFrameIndex below).
  bool requiresFrameIndexScavenging(const MachineFunction &MF) const override {
    return true;
  }
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_RVMIN_RVMINREGISTERINFO_H
