//===-- RVMinFrameLowering.h - RVMin frame lowering ------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_RVMIN_RVMINFRAMELOWERING_H
#define LLVM_LIB_TARGET_RVMIN_RVMINFRAMELOWERING_H

#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/TargetFrameLowering.h"

namespace llvm {

class RegScavenger;
class RVMinInstrInfo;
class RVMinSubtarget;

class RVMinFrameLowering : public TargetFrameLowering {
public:
  explicit RVMinFrameLowering(const RVMinSubtarget &STI);

  void emitPrologue(MachineFunction &MF,
                    MachineBasicBlock &MBB) const override;
  void emitEpilogue(MachineFunction &MF,
                    MachineBasicBlock &MBB) const override;

  bool spillCalleeSavedRegisters(MachineBasicBlock &MBB,
                                 MachineBasicBlock::iterator MI,
                                 ArrayRef<CalleeSavedInfo> CSI,
                                 const TargetRegisterInfo *TRI) const override;
  bool restoreCalleeSavedRegisters(MachineBasicBlock &MBB,
                                   MachineBasicBlock::iterator MI,
                                   MutableArrayRef<CalleeSavedInfo> CSI,
                                   const TargetRegisterInfo *TRI) const override;

  MachineBasicBlock::iterator
  eliminateCallFramePseudoInstr(MachineFunction &MF, MachineBasicBlock &MBB,
                                MachineBasicBlock::iterator MI) const override;

  void processFunctionBeforeFrameFinalized(MachineFunction &MF,
                                           RegScavenger *RS) const override;

  /// Materialize the signed 32-bit constant \p Value into \p Dst with
  /// `LUI` + `ADDI` (a single `ADDI x0, imm` when it fits in 12 bits).
  static void materializeConstant(MachineBasicBlock &MBB,
                                  MachineBasicBlock::iterator I,
                                  const RVMinInstrInfo &TII, const DebugLoc &DL,
                                  Register Dst, int64_t Value,
                                  MachineInstr::MIFlag Flag =
                                      MachineInstr::NoFlags);

  /// Compute \p Dst = \p Base + \p Offset.  Chapter 16 uses this for frame
  /// accesses whose offset does not fit in the 12-bit immediate field.
  static void materializeAddress(MachineBasicBlock &MBB,
                                 MachineBasicBlock::iterator I,
                                 const RVMinInstrInfo &TII, const DebugLoc &DL,
                                 Register Dst, Register Base, int64_t Offset);

private:
  /// Emit `.cfi_def_cfa_offset` and one `.cfi_offset` per callee-saved
  /// register, after the prologue stores.
  void emitPrologueCFI(MachineFunction &MF, MachineBasicBlock &MBB) const;

protected:
  bool hasFPImpl(const MachineFunction &MF) const override;
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_RVMIN_RVMINFRAMELOWERING_H
