//===-- MiniRVFrameLowering.h - MiniRV Frame Information --------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_MINIRV_MINIRVFRAMELOWERING_H
#define LLVM_LIB_TARGET_MINIRV_MINIRVFRAMELOWERING_H

#include "llvm/CodeGen/TargetFrameLowering.h"

namespace llvm {

class MiniRVSubtarget;
class MiniRVInstrInfo;
class MiniRVRegisterInfo;

class MiniRVFrameLowering : public TargetFrameLowering {
  const MiniRVSubtarget &STI;
  const MiniRVInstrInfo &TII;
  const MiniRVRegisterInfo &TRI;

public:
  explicit MiniRVFrameLowering(const MiniRVSubtarget &STI);

  void emitPrologue(MachineFunction &MF, MachineBasicBlock &MBB) const override;
  void emitEpilogue(MachineFunction &MF, MachineBasicBlock &MBB) const override;

  /// The outgoing argument area is reserved in the prologue unless the
  /// function has a variable-length allocation: a dynamic alloca moves the
  /// stack pointer, so the space for the arguments has to be claimed at the
  /// call site instead.
  bool hasReservedCallFrame(const MachineFunction &MF) const override;

  /// A frame index is addressed from the stack pointer when the function has
  /// no frame pointer, and from X8 otherwise.  The callee-saved spill slots
  /// are the exception: they are written before X8 is set up and read after
  /// it has been restored, so they always use the stack pointer.
  StackOffset getFrameIndexReference(const MachineFunction &MF, int FI,
                                     Register &FrameReg) const override;

  void determineCalleeSaves(MachineFunction &MF, BitVector &SavedRegs,
                            RegScavenger *RS) const override;

  /// The callee-saved spills and restores are marked as prologue and epilogue
  /// code.  The prologue relies on the mark to find the point where the frame
  /// pointer may be written, which is after the old value has been saved.
  bool spillCalleeSavedRegisters(MachineBasicBlock &MBB,
                                 MachineBasicBlock::iterator MI,
                                 ArrayRef<CalleeSavedInfo> CSI,
                                 const TargetRegisterInfo *TRI) const override;
  bool restoreCalleeSavedRegisters(MachineBasicBlock &MBB,
                                   MachineBasicBlock::iterator MI,
                                   MutableArrayRef<CalleeSavedInfo> CSI,
                                   const TargetRegisterInfo *TRI) const override;

  /// Add \p Val to \p SrcReg and put the result in \p DestReg.  A small
  /// adjustment is a single addi; anything that does not fit in twelve bits is
  /// materialised with lui+addi and added.  Returns the insertion point just
  /// after the emitted instructions so that the caller can attach CFI state
  /// to the adjusted value.
  MachineBasicBlock::iterator
  adjustReg(MachineBasicBlock &MBB, MachineBasicBlock::iterator II,
            const DebugLoc &DL, Register DestReg, Register SrcReg, int64_t Val,
            MachineInstr::MIFlag Flag) const;

  /// Round the stack pointer down to \p MaxAlign.  Used when an object asks
  /// for more alignment than the ABI stack alignment provides.
  void realignStack(MachineBasicBlock &MBB, MachineBasicBlock::iterator II,
                    const DebugLoc &DL, Align MaxAlign) const;

  MachineBasicBlock::iterator
  eliminateCallFramePseudoInstr(MachineFunction &MF, MachineBasicBlock &MBB,
                                MachineBasicBlock::iterator MI) const override;

protected:
  bool hasFPImpl(const MachineFunction &MF) const override;
};

} // end namespace llvm

#endif // LLVM_LIB_TARGET_MINIRV_MINIRVFRAMELOWERING_H
