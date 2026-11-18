//===-- GPUMinInstrInfo.h - GPUMin instruction information -----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_GPUMIN_GPUMININSTRINFO_H
#define LLVM_LIB_TARGET_GPUMIN_GPUMININSTRINFO_H

#include "GPUMinRegisterInfo.h"
#include "llvm/CodeGen/TargetInstrInfo.h"

#define GET_INSTRINFO_HEADER
#include "GPUMinGenInstrInfo.inc"

namespace llvm {

class GPUMinSubtarget;

class GPUMinInstrInfo : public GPUMinGenInstrInfo {
  const GPUMinRegisterInfo RI;
  virtual void anchor();

public:
  explicit GPUMinInstrInfo(const GPUMinSubtarget &STI);

  const GPUMinRegisterInfo &getRegisterInfo() const { return RI; }

  void copyPhysReg(MachineBasicBlock &MBB, MachineBasicBlock::iterator I,
                   const DebugLoc &DL, Register DestReg, Register SrcReg,
                   bool KillSrc, bool RenamableDest,
                   bool RenamableSrc) const override;

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

  void storeRegToStackSlot(MachineBasicBlock &MBB,
                           MachineBasicBlock::iterator MBBII, Register SrcReg,
                           bool IsKill, int FrameIndex,
                           const TargetRegisterClass *RC, Register VReg,
                           MachineInstr::MIFlag Flags) const override;

  void loadRegFromStackSlot(MachineBasicBlock &MBB,
                            MachineBasicBlock::iterator MBBII,
                            Register DestReg, int FrameIndex,
                            const TargetRegisterClass *RC, Register VReg,
                            unsigned SubReg,
                            MachineInstr::MIFlag Flags) const override;
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_GPUMIN_GPUMININSTRINFO_H
