//===-- MiniRVInstrInfo.cpp - MiniRV Instruction Information --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "MiniRVInstrInfo.h"
#include "MiniRVSubtarget.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"

using namespace llvm;

#define GET_INSTRINFO_CTOR_DTOR
#include "MiniRVGenInstrInfo.inc"

MiniRVInstrInfo::MiniRVInstrInfo(const MiniRVSubtarget &STI)
    : MiniRVGenInstrInfo(STI, RI, /*CallFrameSetupOpcode=*/0,
                         /*CallFrameDestroyOpcode=*/0),
      RI() {}

void MiniRVInstrInfo::copyPhysReg(MachineBasicBlock &MBB,
                                  MachineBasicBlock::iterator MI,
                                  const DebugLoc &DL, Register DestReg,
                                  Register SrcReg, bool KillSrc,
                                  bool RenamableDest, bool RenamableSrc) const {
  // The corpus has no move instruction, but addi rd, rs, 0 is one.
  BuildMI(MBB, MI, DL, get(MiniRV::ADDI), DestReg)
      .addReg(SrcReg, getKillRegState(KillSrc))
      .addImm(0);
}
