//===-- GPUMinFrameLowering.h - GPUMin frame lowering ----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_GPUMIN_GPUMINFRAMELOWERING_H
#define LLVM_LIB_TARGET_GPUMIN_GPUMINFRAMELOWERING_H

#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/TargetFrameLowering.h"

namespace llvm {

class GPUMinSubtarget;

/// Chapter 07 gives GM32 its private stack: an sp-relative frame with no frame
/// pointer, an explicit lr save in functions that call, and the generic
/// callee-saved machinery for r20-r27.
class GPUMinFrameLowering : public TargetFrameLowering {
public:
  explicit GPUMinFrameLowering(const GPUMinSubtarget &STI);

  void emitPrologue(MachineFunction &MF,
                    MachineBasicBlock &MBB) const override;
  void emitEpilogue(MachineFunction &MF,
                    MachineBasicBlock &MBB) const override;

  void determineCalleeSaves(MachineFunction &MF, BitVector &SavedRegs,
                            RegScavenger *RS = nullptr) const override;

protected:
  bool hasFPImpl(const MachineFunction &MF) const override;
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_GPUMIN_GPUMINFRAMELOWERING_H
