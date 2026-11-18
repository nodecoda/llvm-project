//===-- GPUMinInstrInfo.cpp - GPUMin instruction information --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "GPUMinInstrInfo.h"
#include "GPUMinSubtarget.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"

using namespace llvm;

#define GET_INSTRINFO_CTOR_DTOR
#include "GPUMinGenInstrInfo.inc"

void GPUMinInstrInfo::anchor() {}

GPUMinInstrInfo::GPUMinInstrInfo(const GPUMinSubtarget &STI)
    : GPUMinGenInstrInfo(STI, RI), RI() {}

// The register allocator and the calling convention both materialise moves;
// GM32 spells them `mov rd, rs1`.
void GPUMinInstrInfo::copyPhysReg(MachineBasicBlock &MBB,
                                  MachineBasicBlock::iterator I,
                                  const DebugLoc &DL, Register DestReg,
                                  Register SrcReg, bool KillSrc,
                                  bool RenamableDest,
                                  bool RenamableSrc) const {
  if (!GPUMin::GPRRegClass.contains(DestReg, SrcReg))
    llvm_unreachable("GPUMin: only GPR-to-GPR copies are supported");

  BuildMI(MBB, I, DL, get(GPUMin::MOV), DestReg)
      .addReg(SrcReg, getKillRegState(KillSrc));
}
