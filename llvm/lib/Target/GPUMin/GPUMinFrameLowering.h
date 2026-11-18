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

/// Chapter 00 has no stack at all: a kernel has no frame, and device-function
/// calls (chapter 07) are the first thing that needs the private stack.  The
/// prologue and epilogue are therefore empty; the class exists because every
/// target must provide one.
class GPUMinFrameLowering : public TargetFrameLowering {
public:
  explicit GPUMinFrameLowering(const GPUMinSubtarget &STI);

  void emitPrologue(MachineFunction &MF,
                    MachineBasicBlock &MBB) const override;
  void emitEpilogue(MachineFunction &MF,
                    MachineBasicBlock &MBB) const override;

protected:
  bool hasFPImpl(const MachineFunction &MF) const override;
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_GPUMIN_GPUMINFRAMELOWERING_H
