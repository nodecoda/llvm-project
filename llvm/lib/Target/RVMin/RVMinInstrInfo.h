//===-- RVMinInstrInfo.h - RVMin instruction information -------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_RVMIN_RVMININSTRINFO_H
#define LLVM_LIB_TARGET_RVMIN_RVMININSTRINFO_H

#include "RVMinRegisterInfo.h"
#include "llvm/CodeGen/TargetInstrInfo.h"

#define GET_INSTRINFO_HEADER
#include "RVMinGenInstrInfo.inc"

namespace llvm {

class RVMinSubtarget;

class RVMinInstrInfo : public RVMinGenInstrInfo {
  const RVMinRegisterInfo RI;
  virtual void anchor();

public:
  explicit RVMinInstrInfo(const RVMinSubtarget &STI);

  const RVMinRegisterInfo &getRegisterInfo() const { return RI; }

  unsigned getInstSizeInBytes(const MachineInstr &MI) const override;

  void copyPhysReg(MachineBasicBlock &MBB, MachineBasicBlock::iterator MI,
                   const DebugLoc &DL, Register DestReg, Register SrcReg,
                   bool KillSrc, bool RenamableDest = false,
                   bool RenamableSrc = false) const override;

  bool analyzeBranch(MachineBasicBlock &MBB, MachineBasicBlock *&TBB,
                     MachineBasicBlock *&FBB,
                     SmallVectorImpl<MachineOperand> &Cond,
                     bool AllowModify = false) const override;
  unsigned removeBranch(MachineBasicBlock &MBB,
                        int *BytesRemoved = nullptr) const override;
  unsigned insertBranch(MachineBasicBlock &MBB, MachineBasicBlock *TBB,
                        MachineBasicBlock *FBB,
                        ArrayRef<MachineOperand> Cond, const DebugLoc &DL,
                        int *BytesAdded = nullptr) const override;
  bool isUnconditionalBranch(const MachineInstr &MI) const;

  void storeRegToStackSlot(MachineBasicBlock &MBB,
                           MachineBasicBlock::iterator MI, Register SrcReg,
                           bool isKill, int FrameIndex,
                           const TargetRegisterClass *RC, Register VReg,
                           MachineInstr::MIFlag Flags) const override;
  void loadRegFromStackSlot(MachineBasicBlock &MBB,
                            MachineBasicBlock::iterator MI, Register DestReg,
                            int FrameIndex, const TargetRegisterClass *RC,
                            Register VReg, unsigned SubReg,
                            MachineInstr::MIFlag Flags) const override;
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_RVMIN_RVMININSTRINFO_H
