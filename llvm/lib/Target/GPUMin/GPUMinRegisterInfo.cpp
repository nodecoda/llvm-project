//===-- GPUMinRegisterInfo.cpp - GPUMin register information --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "GPUMinRegisterInfo.h"
#include "GPUMinSubtarget.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

#define GET_REGINFO_TARGET_DESC
#include "GPUMinGenRegisterInfo.inc"

GPUMinRegisterInfo::GPUMinRegisterInfo() : GPUMinGenRegisterInfo(GPUMin::R31) {}

// Generated from CSR_GPUMin in GPUMinCallingConv.td: lr (r31) plus s0-s7
// (r20-r27).
const MCPhysReg *
GPUMinRegisterInfo::getCalleeSavedRegs(const MachineFunction *MF) const {
  return CSR_GPUMin_SaveList;
}

const uint32_t *
GPUMinRegisterInfo::getCallPreservedMask(const MachineFunction &MF,
                                         CallingConv::ID CC) const {
  return CSR_GPUMin_RegMask;
}

BitVector GPUMinRegisterInfo::getReservedRegs(const MachineFunction &MF) const {
  BitVector Reserved(getNumRegs());
  // r0 is hardwired to zero; code generation must never allocate it as a
  // destination.  r30 is the private-stack pointer and is owned by the frame
  // lowering (chapter 07); until then it is reserved outright.
  Reserved.set(GPUMin::R0);
  Reserved.set(GPUMin::R30);
  return Reserved;
}

bool GPUMinRegisterInfo::eliminateFrameIndex(MachineBasicBlock::iterator II,
                                             int SPAdj, unsigned FIOperandNum,
                                             RegScavenger *RS) const {
  // Frame index elimination needs the private-stack layout: the GM32 address
  // modes and the r30-relative offsets are chapter 07's subject.  Nothing in
  // chapters 00-06 can create a frame index, so reaching here is a bug.
  llvm_unreachable("GPUMin: frame index elimination arrives in chapter 07");
}

Register GPUMinRegisterInfo::getFrameRegister(const MachineFunction &MF) const {
  return GPUMin::R30;
}
