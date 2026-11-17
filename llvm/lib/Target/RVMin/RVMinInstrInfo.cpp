//===-- RVMinInstrInfo.cpp - RVMin instruction information ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "RVMinInstrInfo.h"
#include "RVMin.h"
#include "RVMinSubtarget.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include <iterator>

using namespace llvm;

#define GET_INSTRINFO_CTOR_DTOR
#include "RVMinGenInstrInfo.inc"

void RVMinInstrInfo::anchor() {}

RVMinInstrInfo::RVMinInstrInfo(const RVMinSubtarget &STI)
    : RVMinGenInstrInfo(STI, RI, RVMin::ADJCALLSTACKDOWN,
                        RVMin::ADJCALLSTACKUP),
      RI() {}

unsigned RVMinInstrInfo::getInstSizeInBytes(const MachineInstr &MI) const {
  if (MI.isDebugInstr() || MI.isCFIInstruction())
    return 0;

  // Chapter 49/50: a data vector instruction may be preceded by an
  // AsmPrinter-inserted vsetivli (4 bytes) when the cached VL/VTYPE is not
  // valid.  The generic AsmPrinter size verifier compares the emitted bytes
  // against this answer, so report the upper bound: 8 when a configuration
  // may be inserted, 4 when the instruction is already a vset*.
  //
  // The AsmPrinter uses the same FmtV marker (TSFlags[4:0] == 17) and the
  // same vset exclusion in isVectorDataInstr(); keep the two in sync.
  if ((MI.getDesc().TSFlags & 0x1f) == 17) {
    unsigned Opc = MI.getOpcode();
    if (Opc != RVMin::VSETIVLI && Opc != RVMin::VSETVLI &&
        Opc != RVMin::VSETVL)
      return 8;
  }

  // Other real RV32I instructions are always 4 bytes.  Pseudos use their
  // declared Size (PseudoRET is 4 after expansion, ADJCALLSTACK* are 0).
  return MI.getDesc().getSize();
}

// RV32I has no move instruction; `addi rd, rs, 0` is the canonical copy.
// Chapter 40 adds the three floating-point cases:
//
//   FPR -> FPR   fsgnj.s rd, rs, rs   (the canonical `fmv.s` alias)
//   FPR -> GPR   fmv.x.w rd, rs
//   GPR -> FPR   fmv.w.x rd, rs
void RVMinInstrInfo::copyPhysReg(MachineBasicBlock &MBB,
                                 MachineBasicBlock::iterator MI,
                                 const DebugLoc &DL, Register DestReg,
                                 Register SrcReg, bool KillSrc,
                                 bool RenamableDest, bool RenamableSrc) const {
  if (RVMin::GPRRegClass.contains(DestReg, SrcReg)) {
    BuildMI(MBB, MI, DL, get(RVMin::ADDI), DestReg)
        .addReg(SrcReg, getKillRegState(KillSrc))
        .addImm(0);
    return;
  }

  // Chapter 48: a vector register copy is `vmv.v.v`.  The AsmPrinter inserts
  // the matching vsetivli immediately before it.
  if (RVMin::VRRegClass.contains(DestReg, SrcReg)) {
    BuildMI(MBB, MI, DL, get(RVMin::VMV_V_V), DestReg)
        .addReg(SrcReg, getKillRegState(KillSrc));
    return;
  }

  // Chapter 41: FPR32 and FPR64 contain the same physical registers, so the
  // feature decides which view a copy uses.  `fsgnj.d` copies all 64 bits,
  // which is also right for an f32 value: RV32D keeps floats NaN-boxed.
  // Query the function's subtarget here: the InstrInfo object is created
  // before ParseSubtargetFeatures runs, so a cached flag would be wrong.
  const bool HasD =
      MBB.getParent()->getSubtarget<RVMinSubtarget>().hasStdExtD();
  if (HasD && RVMin::FPR64RegClass.contains(DestReg, SrcReg)) {
    BuildMI(MBB, MI, DL, get(RVMin::FSGNJ_D), DestReg)
        .addReg(SrcReg, getKillRegState(KillSrc))
        .addReg(SrcReg, getKillRegState(KillSrc));
    return;
  }

  if (RVMin::FPR32RegClass.contains(DestReg, SrcReg)) {
    BuildMI(MBB, MI, DL, get(RVMin::FSGNJ_S), DestReg)
        .addReg(SrcReg, getKillRegState(KillSrc))
        .addReg(SrcReg, getKillRegState(KillSrc));
    return;
  }

  if (RVMin::FPR32RegClass.contains(DestReg) &&
      RVMin::GPRRegClass.contains(SrcReg)) {
    BuildMI(MBB, MI, DL, get(RVMin::FMV_W_X), DestReg)
        .addReg(SrcReg, getKillRegState(KillSrc));
    return;
  }

  if (RVMin::GPRRegClass.contains(DestReg) &&
      RVMin::FPR32RegClass.contains(SrcReg)) {
    BuildMI(MBB, MI, DL, get(RVMin::FMV_X_W), DestReg)
        .addReg(SrcReg, getKillRegState(KillSrc));
    return;
  }

  llvm_unreachable("RVMin: unsupported physical register copy");
}

namespace {

bool isRVMinConditionalBranch(unsigned Opc) {
  switch (Opc) {
  case RVMin::BEQ:
  case RVMin::BNE:
  case RVMin::BLT:
  case RVMin::BGE:
  case RVMin::BLTU:
  case RVMin::BGEU:
    return true;
  default:
    return false;
  }
}

} // end anonymous namespace

bool RVMinInstrInfo::isUnconditionalBranch(const MachineInstr &MI) const {
  if (MI.getOpcode() == RVMin::PseudoBR)
    return true;
  // After pseudo expansion: `jal zero, target`.
  return MI.getOpcode() == RVMin::JAL && MI.getOperand(0).isReg() &&
         MI.getOperand(0).getReg() == RVMin::X0;
}

// BranchFolding and MachineBlockPlacement need to understand the control flow
// at the end of a block.  A block may end with:
//
//   - nothing (fall through),
//   - an unconditional PseudoBR / `jal zero`,
//   - one or two conditional branches (identical conditions may be merged).
//
// The condition is reconstructed in insertBranch from Cond =
// [opcode, rs1, rs2].
bool RVMinInstrInfo::analyzeBranch(MachineBasicBlock &MBB,
                                   MachineBasicBlock *&TBB,
                                   MachineBasicBlock *&FBB,
                                   SmallVectorImpl<MachineOperand> &Cond,
                                   bool AllowModify) const {
  TBB = nullptr;
  FBB = nullptr;
  Cond.clear();

  MachineBasicBlock::iterator I = MBB.end();
  while (I != MBB.begin()) {
    --I;
    if (I->isDebugInstr() || I->isCFIInstruction())
      continue;

    if (!isUnpredicatedTerminator(*I))
      return true;

    if (isUnconditionalBranch(*I)) {
      MachineBasicBlock *Dest = I->getOperand(I->getNumOperands() - 1).getMBB();
      if (!AllowModify) {
        TBB = Dest;
        continue;
      }

      // Anything after the branch is unreachable.
      MBB.erase(std::next(I), MBB.end());
      Cond.clear();
      FBB = nullptr;

      // A branch to the layout successor is a fall-through: delete it.
      if (MBB.isLayoutSuccessor(Dest)) {
        TBB = nullptr;
        I->eraseFromParent();
        I = MBB.end();
        continue;
      }

      TBB = Dest;
      continue;
    }

    if (!isRVMinConditionalBranch(I->getOpcode()))
      return true;
    if (!I->getOperand(I->getNumOperands() - 1).isMBB())
      return true;

    MachineBasicBlock *Dest = I->getOperand(I->getNumOperands() - 1).getMBB();
    if (Cond.empty()) {
      FBB = TBB;
      TBB = Dest;
      Cond.push_back(MachineOperand::CreateImm(I->getOpcode()));
      Cond.push_back(I->getOperand(0));
      Cond.push_back(I->getOperand(1));
      continue;
    }

    // Two conditional branches are only understood when they are identical;
    // this happens with e.g. `if (a == 0 || b)` after block merging.
    if (TBB != Dest || Cond[0].getImm() != I->getOpcode() ||
        !Cond[1].isIdenticalTo(I->getOperand(0)) ||
        !Cond[2].isIdenticalTo(I->getOperand(1)))
      return true;
  }

  return false;
}

unsigned RVMinInstrInfo::removeBranch(MachineBasicBlock &MBB,
                                      int *BytesRemoved) const {
  if (BytesRemoved)
    *BytesRemoved = 0;

  unsigned Count = 0;
  MachineBasicBlock::iterator I = MBB.end();
  while (I != MBB.begin()) {
    --I;
    if (I->isDebugInstr())
      continue;
    if (!isUnconditionalBranch(*I) && !isRVMinConditionalBranch(I->getOpcode()))
      break;
    if (BytesRemoved)
      *BytesRemoved += getInstSizeInBytes(*I);
    I->eraseFromParent();
    I = MBB.end();
    ++Count;
  }
  return Count;
}

unsigned RVMinInstrInfo::insertBranch(MachineBasicBlock &MBB,
                                      MachineBasicBlock *TBB,
                                      MachineBasicBlock *FBB,
                                      ArrayRef<MachineOperand> Cond,
                                      const DebugLoc &DL,
                                      int *BytesAdded) const {
  if (BytesAdded)
    *BytesAdded = 0;

  assert(TBB && "insertBranch must not be told to insert a fallthrough");
  assert((Cond.size() == 3 || Cond.empty()) &&
         "RVMin branch conditions have two register operands");

  if (Cond.empty()) {
    assert(!FBB && "unconditional branch with multiple successors");
    MachineInstr &MI = *BuildMI(&MBB, DL, get(RVMin::PseudoBR)).addMBB(TBB);
    if (BytesAdded)
      *BytesAdded += getInstSizeInBytes(MI);
    return 1;
  }

  MachineInstr &CondMI = *BuildMI(&MBB, DL, get(Cond[0].getImm()))
                              .add(Cond[1])
                              .add(Cond[2])
                              .addMBB(TBB);
  unsigned Count = 1;
  if (BytesAdded)
    *BytesAdded += getInstSizeInBytes(CondMI);

  if (FBB) {
    MachineInstr &MI = *BuildMI(&MBB, DL, get(RVMin::PseudoBR)).addMBB(FBB);
    if (BytesAdded)
      *BytesAdded += getInstSizeInBytes(MI);
    ++Count;
  }
  return Count;
}

// Spill/reload for register allocation and for callee-saved registers.  The
// frame index is left as an operand: PEI's eliminateFrameIndex turns it into
// an sp-relative offset once the frame layout is final.
void RVMinInstrInfo::storeRegToStackSlot(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MI, Register SrcReg,
    bool IsKill, int FrameIndex, const TargetRegisterClass *RC, Register VReg,
    MachineInstr::MIFlag Flags) const {
  // Chapter 41: doubles spill with fsd, floats with fsw, integers with sw.
  // Chapter 48: a 128-bit vector spills with vse32.v.  Vector load/store have
  // no immediate offset operand, so the frame index is the last operand and
  // eliminateFrameIndex rewrites it to (sp) or a materialized scratch GPR.
  unsigned Opc = RVMin::SW;
  bool IsVector = false;
  if (RC == &RVMin::VRRegClass) {
    Opc = RVMin::VSE32_V;
    IsVector = true;
  } else if (RC == &RVMin::FPR64RegClass)
    Opc = RVMin::FSD;
  else if (RC == &RVMin::FPR32RegClass)
    Opc = RVMin::FSW;
  else
    assert(RC == &RVMin::GPRRegClass &&
           "RVMin: unexpected register class in spill");
  DebugLoc DL = MI != MBB.end() ? MI->getDebugLoc() : DebugLoc();
  MachineInstrBuilder MIB = BuildMI(MBB, MI, DL, get(Opc))
                                .addReg(SrcReg, getKillRegState(IsKill))
                                .addFrameIndex(FrameIndex);
  if (!IsVector)
    MIB.addImm(0);
  if (Flags != MachineInstr::NoFlags)
    MIB->setFlag(Flags);
}

void RVMinInstrInfo::loadRegFromStackSlot(MachineBasicBlock &MBB,
                                          MachineBasicBlock::iterator MI,
                                          Register DestReg, int FrameIndex,
                                          const TargetRegisterClass *RC,
                                          Register VReg, unsigned SubReg,
                                          MachineInstr::MIFlag Flags) const {
  unsigned Opc = RVMin::LW;
  bool IsVector = false;
  if (RC == &RVMin::VRRegClass) {
    Opc = RVMin::VLE32_V;
    IsVector = true;
  } else if (RC == &RVMin::FPR64RegClass)
    Opc = RVMin::FLD;
  else if (RC == &RVMin::FPR32RegClass)
    Opc = RVMin::FLW;
  else
    assert(RC == &RVMin::GPRRegClass &&
           "RVMin: unexpected register class in reload");
  assert(SubReg == 0 && "RVMin has no sub-register reloads");
  DebugLoc DL = MI != MBB.end() ? MI->getDebugLoc() : DebugLoc();
  MachineInstrBuilder MIB =
      BuildMI(MBB, MI, DL, get(Opc), DestReg).addFrameIndex(FrameIndex);
  if (!IsVector)
    MIB.addImm(0);
  if (Flags != MachineInstr::NoFlags)
    MIB->setFlag(Flags);
}
