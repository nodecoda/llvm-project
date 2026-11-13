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

  bool hasReservedCallFrame(const MachineFunction &MF) const override {
    return true;
  }

  MachineBasicBlock::iterator
  eliminateCallFramePseudoInstr(MachineFunction &MF, MachineBasicBlock &MBB,
                                MachineBasicBlock::iterator MI) const override;

protected:
  bool hasFPImpl(const MachineFunction &MF) const override;
};

} // end namespace llvm

#endif // LLVM_LIB_TARGET_MINIRV_MINIRVFRAMELOWERING_H
