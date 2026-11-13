//===-- MiniRVInstrInfo.h - MiniRV Instruction Information ------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_MINIRV_MINIRVINSTRINFO_H
#define LLVM_LIB_TARGET_MINIRV_MINIRVINSTRINFO_H

#include "MiniRVRegisterInfo.h"
#include "llvm/CodeGen/TargetInstrInfo.h"

#define GET_INSTRINFO_HEADER
#include "MiniRVGenInstrInfo.inc"

namespace llvm {

class MiniRVSubtarget;

class MiniRVInstrInfo : public MiniRVGenInstrInfo {
  const MiniRVRegisterInfo RI;

public:
  explicit MiniRVInstrInfo(const MiniRVSubtarget &STI);

  const MiniRVRegisterInfo &getRegisterInfo() const { return RI; }

  bool analyzeBranch(MachineBasicBlock &MBB, MachineBasicBlock *&TBB,
                     MachineBasicBlock *&FBB,
                     SmallVectorImpl<MachineOperand> &Cond,
                     bool AllowModify = false) const override;

  unsigned removeBranch(MachineBasicBlock &MBB,
                        int *BytesRemoved = nullptr) const override;

  unsigned insertBranch(MachineBasicBlock &MBB, MachineBasicBlock *TBB,
                        MachineBasicBlock *FBB, ArrayRef<MachineOperand> Cond,
                        const DebugLoc &DL,
                        int *BytesAdded = nullptr) const override;

  unsigned getInstSizeInBytes(const MachineInstr &MI) const override;

  MachineBasicBlock *getBranchDestBlock(const MachineInstr &MI) const override;

  /// The conditional branches carry a signed 13-bit byte displacement (the
  /// low bit is implicit) and jal a signed 21-bit one.  BranchRelaxation uses
  /// this to split a block when the target is out of reach.
  bool isBranchOffsetInRange(unsigned BranchOp,
                             int64_t BrOffset) const override;

  /// Only the six comparison records can be inverted; the inverted form is
  /// the record with the opposite predicate.
  bool reverseBranchCondition(
      SmallVectorImpl<MachineOperand> &Cond) const override;

  /// A jal cannot reach beyond +-1 MiB, so a longer jump is materialised as
  /// "la x5, block" followed by "jr x5".  X5 is reserved, and frame indices
  /// are already gone by the time BranchRelaxation runs, so the scratch is
  /// free and no restore block is needed.
  void insertIndirectBranch(MachineBasicBlock &MBB, MachineBasicBlock &DestBB,
                            MachineBasicBlock &RestoreBB, const DebugLoc &DL,
                            int64_t BrOffset = 0,
                            RegScavenger *RS = nullptr) const override;

  void copyPhysReg(MachineBasicBlock &MBB, MachineBasicBlock::iterator MI,
                   const DebugLoc &DL, Register DestReg, Register SrcReg,
                   bool KillSrc, bool RenamableDest = false,
                   bool RenamableSrc = false) const override;

  void storeRegToStackSlot(
      MachineBasicBlock &MBB, MachineBasicBlock::iterator MI, Register SrcReg,
      bool IsKill, int FrameIndex, const TargetRegisterClass *RC, Register VReg,
      MachineInstr::MIFlag Flags = MachineInstr::NoFlags) const override;

  void loadRegFromStackSlot(
      MachineBasicBlock &MBB, MachineBasicBlock::iterator MI, Register DestReg,
      int FrameIndex, const TargetRegisterClass *RC, Register VReg,
      unsigned SubReg = 0,
      MachineInstr::MIFlag Flags = MachineInstr::NoFlags) const override;
};

} // end namespace llvm

#endif // LLVM_LIB_TARGET_MINIRV_MINIRVINSTRINFO_H
