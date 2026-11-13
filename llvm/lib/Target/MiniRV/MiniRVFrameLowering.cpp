//===-- MiniRVFrameLowering.cpp - MiniRV Frame Information ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A frame that fits in a 12-bit signed immediate is adjusted with a single
// addi.  Larger frames materialise the adjustment in a scratch register with
// lui+addi and then add it, which is the same sequence the compiler uses to
// materialise any other 32-bit constant.
//
//===----------------------------------------------------------------------===//

#include "MiniRVFrameLowering.h"
#include "MiniRVInstrInfo.h"
#include "MiniRVSubtarget.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

MiniRVFrameLowering::MiniRVFrameLowering(const MiniRVSubtarget &STI)
    // Match the S128 stack alignment the data layout advertises: every call
    // keeps the stack pointer a multiple of sixteen.
    : TargetFrameLowering(TargetFrameLowering::StackGrowsDown, Align(16), 0,
                          Align(16)),
      STI(STI), TII(*STI.getInstrInfo()), TRI(*STI.getRegisterInfo()) {}

bool MiniRVFrameLowering::hasReservedCallFrame(
    const MachineFunction &MF) const {
  // A dynamic alloca moves the stack pointer, so the outgoing argument area
  // cannot be reserved up front: the pseudo instructions at the call sites
  // claim it instead.
  return !MF.getFrameInfo().hasVarSizedObjects();
}

bool MiniRVFrameLowering::hasFPImpl(const MachineFunction &MF) const {
  // Without a variable-length allocation the stack pointer never moves, so
  // frame indices can stay sp-relative and a frame pointer would only cost a
  // register.  A dynamic alloca changes the stack pointer in the middle of the
  // function, and the objects laid out by the prologue can only be reached
  // through a second base register.  X8 (s0) is that register.  The same
  // register answers llvm.frameaddress, whose value is the incoming stack
  // pointer and therefore cannot be recomputed from the stack pointer before
  // the frame size is known.
  return MF.getFrameInfo().hasVarSizedObjects() ||
         MF.getFrameInfo().isFrameAddressTaken() ||
         MF.getSubtarget().getRegisterInfo()->hasStackRealignment(MF);
}

/// A function with a frame pointer keeps the frame pointer at the incoming
/// stack pointer, so the objects the frame allocator placed below the incoming
/// stack pointer are at their plain object offset, and the incoming arguments
/// above it are too.  Without a frame pointer the stack pointer sits at the
/// bottom of the frame and every object is that much higher.
StackOffset
MiniRVFrameLowering::getFrameIndexReference(const MachineFunction &MF, int FI,
                                            Register &FrameReg) const {
  const MachineFrameInfo &MFI = MF.getFrameInfo();
  if (!hasFP(MF) || MFI.isCalleeSavedObjectIndex(FI)) {
    FrameReg = MiniRV::X2;
    return StackOffset::getFixed(MFI.getObjectOffset(FI) + MFI.getStackSize());
  }

  // When the stack is realigned, the prologue rounds the stack pointer down
  // after setting the frame pointer.  The locals move down with it, so they
  // are addressed from the rounded stack pointer; the frame pointer is only
  // used to recompute the unrounded stack pointer and to reach the incoming
  // arguments.
  if (MF.getSubtarget().getRegisterInfo()->hasStackRealignment(MF) &&
      !MFI.isFixedObjectIndex(FI)) {
    FrameReg = MiniRV::X2;
    return StackOffset::getFixed(MFI.getObjectOffset(FI) + MFI.getStackSize());
  }

  FrameReg = MiniRV::X8;
  return StackOffset::getFixed(MFI.getObjectOffset(FI));
}

bool MiniRVFrameLowering::spillCalleeSavedRegisters(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MI,
    ArrayRef<CalleeSavedInfo> CSI, const TargetRegisterInfo *TRI) const {
  // llvm.returnaddress reads X1 after the prologue has spilled it, so the
  // spill cannot claim to kill the incoming value in that case; the machine
  // verifier would reject the later use of a killed register.
  bool RetAddrTaken = MBB.getParent()->getFrameInfo().isReturnAddressTaken();
  for (const CalleeSavedInfo &CS : CSI) {
    Register Reg = CS.getReg();
    const TargetRegisterClass *RC = TRI->getMinimalPhysRegClass(Reg);
    bool IsKill = !(Reg == MiniRV::X1 && RetAddrTaken);
    TII.storeRegToStackSlot(MBB, MI, Reg, IsKill, CS.getFrameIdx(), RC,
                            Register(), MachineInstr::FrameSetup);
  }
  return true;
}

bool MiniRVFrameLowering::restoreCalleeSavedRegisters(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MI,
    MutableArrayRef<CalleeSavedInfo> CSI, const TargetRegisterInfo *TRI) const {
  for (const CalleeSavedInfo &CS : reverse(CSI)) {
    Register Reg = CS.getReg();
    const TargetRegisterClass *RC = TRI->getMinimalPhysRegClass(Reg);
    TII.loadRegFromStackSlot(MBB, MI, Reg, CS.getFrameIdx(), RC, Register(),
                             /*SubReg=*/0, MachineInstr::FrameDestroy);
  }
  return true;
}

void MiniRVFrameLowering::determineCalleeSaves(MachineFunction &MF,
                                               BitVector &SavedRegs,
                                               RegScavenger *RS) const {
  TargetFrameLowering::determineCalleeSaves(MF, SavedRegs, RS);

  // The prologue overwrites the frame pointer, but that happens after the
  // callee-saved spills have been placed, so the register machinery cannot
  // see the clobber and has to be told about it.
  if (hasFP(MF))
    SavedRegs.set(MiniRV::X8);
}

void MiniRVFrameLowering::emitPrologue(MachineFunction &MF,
                                       MachineBasicBlock &MBB) const {
  const MachineFrameInfo &MFI = MF.getFrameInfo();
  uint64_t StackSize = MFI.getStackSize();
  bool HasFP = hasFP(MF);
  if (!StackSize && !HasFP)
    return;

  if (!isInt<32>(StackSize))
    report_fatal_error("MiniRV frames must fit in a 32-bit immediate");
  DebugLoc DL =
      MBB.begin() != MBB.end() ? MBB.begin()->getDebugLoc() : DebugLoc();

  // The adjustment has to be the first thing in the function: the callee-saved
  // spills that PEI inserts afterwards use offsets relative to the new SP.
  if (StackSize)
    adjustReg(MBB, MBB.begin(), DL, MiniRV::X2, MiniRV::X2,
              -static_cast<int64_t>(StackSize), MachineInstr::FrameSetup);

  if (!HasFP)
    return;

  // The frame pointer is set to the incoming stack pointer and must be
  // written after the old value has been spilled, so it goes behind the
  // frame-setup instructions that PEI has already placed at the top of the
  // block.
  MachineBasicBlock::iterator InsertPt = MBB.begin();
  while (InsertPt != MBB.end() &&
         InsertPt->getFlag(MachineInstr::FrameSetup))
    ++InsertPt;
  adjustReg(MBB, InsertPt, DL, MiniRV::X8, MiniRV::X2,
            static_cast<int64_t>(StackSize), MachineInstr::FrameSetup);

  // An object that asks for more than the ABI stack alignment needs the stack
  // pointer rounded down.  This runs after the callee-saved spills, which are
  // addressed relative to the unrounded SP; the epilogue restores that SP from
  // the frame pointer before reloading them.
  if (TRI.hasStackRealignment(MF)) {
    // A variable-length allocation moves the stack pointer after the
    // realignment, so a second base register would be needed to reach the
    // locals.  Reject the combination rather than miscompile it.
    if (MFI.hasVarSizedObjects())
      report_fatal_error(
          "MiniRV cannot realign the stack with variable-sized allocations");
    realignStack(MBB, InsertPt, DL, MFI.getMaxAlign());
  }
}

void MiniRVFrameLowering::realignStack(MachineBasicBlock &MBB,
                                       MachineBasicBlock::iterator II,
                                       const DebugLoc &DL,
                                       Align MaxAlign) const {
  int64_t Mask = -static_cast<int64_t>(MaxAlign.value());
  if (isInt<12>(Mask)) {
    BuildMI(MBB, II, DL, TII.get(MiniRV::ANDI), MiniRV::X2)
        .addReg(MiniRV::X2)
        .addImm(Mask)
        .setMIFlag(MachineInstr::FrameSetup);
    return;
  }

  // -MaxAlign is a 32-bit value with the low bits clear, so lui+addi can
  // materialise it.  X6 is the scratch register adjustReg uses for the same
  // reason: the destination still has to be read.
  assert(isInt<32>(Mask) && "MiniRV cannot align the stack past 2 GiB");
  int64_t Hi20 = (Mask + 0x800) >> 12;
  int64_t Lo12 = Mask - (Hi20 << 12);
  BuildMI(MBB, II, DL, TII.get(MiniRV::LUI), MiniRV::X6)
      .addImm(Hi20)
      .setMIFlag(MachineInstr::FrameSetup);
  BuildMI(MBB, II, DL, TII.get(MiniRV::ADDI), MiniRV::X6)
      .addReg(MiniRV::X6)
      .addImm(Lo12)
      .setMIFlag(MachineInstr::FrameSetup);
  BuildMI(MBB, II, DL, TII.get(MiniRV::AND), MiniRV::X2)
      .addReg(MiniRV::X2)
      .addReg(MiniRV::X6)
      .setMIFlag(MachineInstr::FrameSetup);
}

void MiniRVFrameLowering::emitEpilogue(MachineFunction &MF,
                                       MachineBasicBlock &MBB) const {
  uint64_t StackSize = MF.getFrameInfo().getStackSize();

  if (hasFP(MF)) {
    // The stack pointer may have been moved by a dynamic alloca, so the
    // bottom of the fixed frame is recomputed from the frame pointer.  The
    // callee-saved restores address their slots through the stack pointer,
    // so this has to happen before them.
    MachineBasicBlock::iterator MBBI = MBB.getFirstTerminator();
    while (MBBI != MBB.begin() &&
           std::prev(MBBI)->getFlag(MachineInstr::FrameDestroy))
      --MBBI;
    DebugLoc DL = MBBI != MBB.end() ? MBBI->getDebugLoc() : DebugLoc();
    adjustReg(MBB, MBBI, DL, MiniRV::X2, MiniRV::X8,
              -static_cast<int64_t>(StackSize), MachineInstr::FrameDestroy);
  }

  if (!StackSize)
    return;

  // The restores of the callee-saved registers sit between the adjustment and
  // the return, so the SP must be released after them.
  MachineBasicBlock::iterator MBBI = MBB.getLastNonDebugInstr();
  DebugLoc DL = MBBI->getDebugLoc();
  adjustReg(MBB, MBBI, DL, MiniRV::X2, MiniRV::X2,
            static_cast<int64_t>(StackSize), MachineInstr::FrameDestroy);
}

void MiniRVFrameLowering::adjustReg(MachineBasicBlock &MBB,
                                    MachineBasicBlock::iterator II,
                                    const DebugLoc &DL, Register DestReg,
                                    Register SrcReg, int64_t Val,
                                    MachineInstr::MIFlag Flag) const {
  if (DestReg == SrcReg && Val == 0)
    return;

  if (isInt<12>(Val)) {
    BuildMI(MBB, II, DL, TII.get(MiniRV::ADDI), DestReg)
        .addReg(SrcReg)
        .addImm(Val)
        .setMIFlag(Flag);
    return;
  }

  assert(isInt<32>(Val) && "MiniRV cannot materialise a larger adjustment");
  // Round the high part away from zero so that the low part always fits in
  // the signed twelve bits of addi.  When the destination is also the source
  // the materialised constant cannot be built in place -- lui would clobber
  // the value that still has to be added -- so it goes through a scratch
  // register.
  int64_t Hi20 = (Val + 0x800) >> 12;
  int64_t Lo12 = Val - (Hi20 << 12);
  Register Scratch = DestReg == SrcReg ? Register(MiniRV::X6) : DestReg;
  BuildMI(MBB, II, DL, TII.get(MiniRV::LUI), Scratch)
      .addImm(Hi20)
      .setMIFlag(Flag);
  BuildMI(MBB, II, DL, TII.get(MiniRV::ADDI), Scratch)
      .addReg(Scratch)
      .addImm(Lo12)
      .setMIFlag(Flag);
  BuildMI(MBB, II, DL, TII.get(MiniRV::ADD), DestReg)
      .addReg(Scratch)
      .addReg(SrcReg)
      .setMIFlag(Flag);
}

MachineBasicBlock::iterator MiniRVFrameLowering::eliminateCallFramePseudoInstr(
    MachineFunction &MF, MachineBasicBlock &MBB,
    MachineBasicBlock::iterator MI) const {
  // With a reserved call frame the outgoing argument area is already part of
  // the prologue, so the pseudo has no work to do.  Otherwise -- a dynamic
  // alloca moves the stack pointer, so the space cannot be reserved up front
  // -- the pseudo becomes the adjustment itself.
  if (!hasReservedCallFrame(MF)) {
    // The outgoing argument area is claimed here, so this is the only place
    // that can keep the stack pointer a multiple of the ABI alignment.  Round
    // the area up; the same rounded amount is released by the matching
    // ADJCALLSTACKUP.
    int64_t Amount = alignTo(MI->getOperand(0).getImm(), getStackAlign());
    if (Amount) {
      if (MI->getOpcode() == MiniRV::ADJCALLSTACKDOWN)
        Amount = -Amount;
      adjustReg(MBB, MI, MI->getDebugLoc(), MiniRV::X2, MiniRV::X2, Amount,
                MachineInstr::NoFlags);
    }
  }
  return MBB.erase(MI);
}
