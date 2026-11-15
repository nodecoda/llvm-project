//===-- RVMinFrameLowering.cpp - RVMin frame lowering --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "RVMinFrameLowering.h"
#include "RVMin.h"
#include "RVMinInstrInfo.h"
#include "RVMinSubtarget.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

RVMinFrameLowering::RVMinFrameLowering(const RVMinSubtarget &STI)
    : TargetFrameLowering(StackGrowsDown, Align(16), 0, Align(16)) {}

bool RVMinFrameLowering::hasFPImpl(const MachineFunction &MF) const {
  return false;
}

namespace {

/// `sp += Amount`, where Amount is a *signed* byte count.  RV32I's ADDI covers
/// [-2048, 2047]; larger frames are materialized in t0 first.  t0 is free here
/// because arguments only arrive in a0-a7 and nothing is live yet in the
/// prologue (or anything after the saves in the epilogue).
void adjustStackPointer(MachineBasicBlock &MBB, MachineBasicBlock::iterator I,
                        const RVMinInstrInfo &TII, const DebugLoc &DL,
                        int64_t Amount, MachineInstr::MIFlag Flag) {
  auto emit = [&](unsigned Opc, Register Dst, Register Src) {
    MachineInstr *MI = BuildMI(MBB, I, DL, TII.get(Opc), Dst).addReg(Src);
    MI->setFlag(Flag);
    return MI;
  };

  if (isInt<12>(Amount)) {
    MachineInstr *MI = emit(RVMin::ADDI, RVMin::X2, RVMin::X2);
    MI->addOperand(MachineOperand::CreateImm(Amount));
    return;
  }

  // Materialize |Amount| in t0 with LUI+ADDI, then add/subtract it.
  int64_t Value = Amount < 0 ? -Amount : Amount;
  int64_t Hi = (Value + 0x800) >> 12;
  int64_t Lo = Value - (Hi << 12);
  assert(isUInt<20>(Hi) && "stack size must fit in 31 bits");

  BuildMI(MBB, I, DL, TII.get(RVMin::LUI), RVMin::X5)
      .addImm(Hi)
      .setMIFlag(Flag);
  if (Lo)
    BuildMI(MBB, I, DL, TII.get(RVMin::ADDI), RVMin::X5)
        .addReg(RVMin::X5)
        .addImm(Lo)
        .setMIFlag(Flag);
  // Positive amounts are the epilogue (sp += N), negative ones the prologue.
  BuildMI(MBB, I, DL, TII.get(Amount > 0 ? RVMin::ADD : RVMin::SUB), RVMin::X2)
      .addReg(RVMin::X2)
      .addReg(RVMin::X5)
      .setMIFlag(Flag);
}

} // end anonymous namespace

void RVMinFrameLowering::emitPrologue(MachineFunction &MF,
                                      MachineBasicBlock &MBB) const {
  const MachineFrameInfo &MFI = MF.getFrameInfo();
  uint64_t StackSize = MFI.getStackSize();
  if (!StackSize)
    return;

  const RVMinInstrInfo &TII =
      *static_cast<const RVMinInstrInfo *>(MF.getSubtarget().getInstrInfo());
  DebugLoc DL = MBB.begin() != MBB.end() ? MBB.begin()->getDebugLoc()
                                         : DebugLoc();

  // PEI inserts callee-saved spills at MBB.begin() before calling us, so
  // inserting here keeps the order `sp -= N; sw ...` correct.
  adjustStackPointer(MBB, MBB.begin(), TII, DL, -static_cast<int64_t>(StackSize),
                     MachineInstr::FrameSetup);
}

void RVMinFrameLowering::emitEpilogue(MachineFunction &MF,
                                      MachineBasicBlock &MBB) const {
  const MachineFrameInfo &MFI = MF.getFrameInfo();
  uint64_t StackSize = MFI.getStackSize();
  if (!StackSize)
    return;

  const RVMinInstrInfo &TII =
      *static_cast<const RVMinInstrInfo *>(MF.getSubtarget().getInstrInfo());
  DebugLoc DL = MBB.getFirstTerminator() != MBB.end()
                    ? MBB.getFirstTerminator()->getDebugLoc()
                    : DebugLoc();

  // Callee-saved reloads were inserted before the terminator already, so the
  // stack adjustment goes after them and before the return.
  adjustStackPointer(MBB, MBB.getFirstTerminator(), TII, DL,
                     static_cast<int64_t>(StackSize),
                     MachineInstr::FrameDestroy);
}

//===----------------------------------------------------------------------===//
// Callee-saved registers
//===----------------------------------------------------------------------===//
//
// PEI has already created a stack slot for every register in CSR_RVMin that
// this function modifies (for a non-leaf function that is `ra`; a function
// that uses s0-s11 gets those too).  All that is left is the code that stores
// and reloads them around the body.
//
// The spills are inserted at the beginning of the entry block, the reloads
// before each return.  emitPrologue/emitEpilogue then place the sp adjustment
// outside them, so the sequence is:
//
//   addi sp, sp, -N
//   sw   ra, ...(sp)        <- spill
//   ...
//   lw   ra, ...(sp)        <- reload
//   addi sp, sp, N
//   jalr zero, 0(ra)

bool RVMinFrameLowering::spillCalleeSavedRegisters(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MI,
    ArrayRef<CalleeSavedInfo> CSI, const TargetRegisterInfo *TRI) const {
  const RVMinInstrInfo &TII =
      *static_cast<const RVMinInstrInfo *>(MBB.getParent()->getSubtarget()
                                               .getInstrInfo());

  for (const CalleeSavedInfo &CS : CSI)
    TII.storeRegToStackSlot(MBB, MI, CS.getReg(), /*isKill=*/true,
                            CS.getFrameIdx(), &RVMin::GPRRegClass, Register(),
                            MachineInstr::FrameSetup);
  return true;
}

bool RVMinFrameLowering::restoreCalleeSavedRegisters(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MI,
    MutableArrayRef<CalleeSavedInfo> CSI,
    const TargetRegisterInfo *TRI) const {
  const RVMinInstrInfo &TII =
      *static_cast<const RVMinInstrInfo *>(MBB.getParent()->getSubtarget()
                                               .getInstrInfo());

  for (const CalleeSavedInfo &CS : CSI)
    TII.loadRegFromStackSlot(MBB, MI, CS.getReg(), CS.getFrameIdx(),
                             &RVMin::GPRRegClass, Register(), /*SubReg=*/0,
                             MachineInstr::FrameDestroy);
  return true;
}

//===----------------------------------------------------------------------===//
// Call frame pseudos
//===----------------------------------------------------------------------===//

// ADJCALLSTACKDOWN/UP bracket every call in the DAG.  Because RVMin has no
// frame pointer, hasReservedCallFrame() returns true: PEI already counted the
// largest outgoing argument area into the stack frame, so sp never moves at a
// call site and both pseudos simply disappear.
MachineBasicBlock::iterator RVMinFrameLowering::eliminateCallFramePseudoInstr(
    MachineFunction &MF, MachineBasicBlock &MBB,
    MachineBasicBlock::iterator MI) const {
  assert(hasReservedCallFrame(MF) && "unexpected dynamic call frame");
  return MBB.erase(MI);
}
