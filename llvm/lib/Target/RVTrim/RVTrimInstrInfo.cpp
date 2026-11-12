//===-- RVTrimInstrInfo.cpp - RISC-V Instruction Information -----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains the RISC-V implementation of the TargetInstrInfo class.
//
//===----------------------------------------------------------------------===//

#include "RVTrimInstrInfo.h"
#include "MCTargetDesc/RVTrimBaseInfo.h"
#include "MCTargetDesc/RVTrimMatInt.h"
#include "RVTrim.h"
#include "RVTrimMachineFunctionInfo.h"
#include "RVTrimSubtarget.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Analysis/MemoryLocation.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/CodeGen/LiveIntervals.h"
#include "llvm/CodeGen/MachineCombinerPattern.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/MachineTraceMetrics.h"
#include "llvm/CodeGen/RegisterScavenging.h"
#include "llvm/CodeGen/StackMaps.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/MCDwarf.h"
#include "llvm/MC/MCInstBuilder.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

#define GEN_CHECK_COMPRESS_INSTR
#include "RVTrimGenCompressInstEmitter.inc"

#define GET_INSTRINFO_CTOR_DTOR
#include "RVTrimGenInstrInfo.inc"

#define DEBUG_TYPE "rvtrim-instr-info"
STATISTIC(NumVRegSpilled,
          "Number of registers within vector register groups spilled");
STATISTIC(NumVRegReloaded,
          "Number of registers within vector register groups reloaded");

namespace llvm::RVTrimVPseudosTable {

using namespace RVTrim;

#define GET_RVTrimVPseudosTable_IMPL
#include "RVTrimGenSearchableTables.inc"

} // namespace llvm::RVTrimVPseudosTable

namespace llvm::RVTrim {

#define GET_RVTrimMaskedPseudosTable_IMPL
#include "RVTrimGenSearchableTables.inc"

} // end namespace llvm::RVTrim

RVTrimInstrInfo::RVTrimInstrInfo(const RVTrimSubtarget &STI)
    : RVTrimGenInstrInfo(STI, RegInfo, RVTrim::ADJCALLSTACKDOWN,
                        RVTrim::ADJCALLSTACKUP),
      RegInfo(STI.getHwMode()), STI(STI) {}

#define GET_INSTRINFO_HELPERS
#include "RVTrimGenInstrInfo.inc"

MCInst RVTrimInstrInfo::getNop() const {
  if (STI.hasStdExtZca())
    return MCInstBuilder(RVTrim::C_NOP);
  return MCInstBuilder(RVTrim::ADDI)
      .addReg(RVTrim::X0)
      .addReg(RVTrim::X0)
      .addImm(0);
}

Register RVTrimInstrInfo::isLoadFromStackSlot(const MachineInstr &MI,
                                             int &FrameIndex) const {
  TypeSize Dummy = TypeSize::getZero();
  return isLoadFromStackSlot(MI, FrameIndex, Dummy);
}

static std::optional<unsigned> getLMULForRVVWholeLoadStore(unsigned Opcode) {
  switch (Opcode) {
  default:
    return std::nullopt;
  case RVTrim::VS1R_V:
  case RVTrim::VL1RE8_V:
  case RVTrim::VL1RE16_V:
  case RVTrim::VL1RE32_V:
  case RVTrim::VL1RE64_V:
    return 1;
  case RVTrim::VS2R_V:
  case RVTrim::VL2RE8_V:
  case RVTrim::VL2RE16_V:
  case RVTrim::VL2RE32_V:
  case RVTrim::VL2RE64_V:
    return 2;
  case RVTrim::VS4R_V:
  case RVTrim::VL4RE8_V:
  case RVTrim::VL4RE16_V:
  case RVTrim::VL4RE32_V:
  case RVTrim::VL4RE64_V:
    return 4;
  case RVTrim::VS8R_V:
  case RVTrim::VL8RE8_V:
  case RVTrim::VL8RE16_V:
  case RVTrim::VL8RE32_V:
  case RVTrim::VL8RE64_V:
    return 8;
  }
}

Register RVTrimInstrInfo::isLoadFromStackSlot(const MachineInstr &MI,
                                             int &FrameIndex,
                                             TypeSize &MemBytes) const {
  switch (MI.getOpcode()) {
  default:
    return 0;
  case RVTrim::LB:
  case RVTrim::LBU:
    MemBytes = TypeSize::getFixed(1);
    break;
  case RVTrim::LH:
  case RVTrim::LH_INX:
  case RVTrim::LHU:
  case RVTrim::FLH:
    MemBytes = TypeSize::getFixed(2);
    break;
  case RVTrim::LW:
  case RVTrim::LW_INX:
  case RVTrim::FLW:
  case RVTrim::LWU:
    MemBytes = TypeSize::getFixed(4);
    break;
  case RVTrim::LD:
  case RVTrim::LD_RV32:
  case RVTrim::FLD:
    MemBytes = TypeSize::getFixed(8);
    break;
  case RVTrim::VL1RE8_V:
  case RVTrim::VL2RE8_V:
  case RVTrim::VL4RE8_V:
  case RVTrim::VL8RE8_V:
    if (!MI.getOperand(1).isFI())
      return Register();
    FrameIndex = MI.getOperand(1).getIndex();
    unsigned LMUL = *getLMULForRVVWholeLoadStore(MI.getOpcode());
    MemBytes = TypeSize::getScalable(RISCV::RVVBytesPerBlock * LMUL);
    return MI.getOperand(0).getReg();
  }

  if (MI.getOperand(1).isFI() && MI.getOperand(2).isImm() &&
      MI.getOperand(2).getImm() == 0) {
    FrameIndex = MI.getOperand(1).getIndex();
    return MI.getOperand(0).getReg();
  }

  return 0;
}

Register RVTrimInstrInfo::isStoreToStackSlot(const MachineInstr &MI,
                                            int &FrameIndex) const {
  TypeSize Dummy = TypeSize::getZero();
  return isStoreToStackSlot(MI, FrameIndex, Dummy);
}

Register RVTrimInstrInfo::isStoreToStackSlot(const MachineInstr &MI,
                                            int &FrameIndex,
                                            TypeSize &MemBytes) const {
  switch (MI.getOpcode()) {
  default:
    return 0;
  case RVTrim::SB:
    MemBytes = TypeSize::getFixed(1);
    break;
  case RVTrim::SH:
  case RVTrim::SH_INX:
  case RVTrim::FSH:
    MemBytes = TypeSize::getFixed(2);
    break;
  case RVTrim::SW:
  case RVTrim::SW_INX:
  case RVTrim::FSW:
    MemBytes = TypeSize::getFixed(4);
    break;
  case RVTrim::SD:
  case RVTrim::SD_RV32:
  case RVTrim::FSD:
    MemBytes = TypeSize::getFixed(8);
    break;
  case RVTrim::VS1R_V:
  case RVTrim::VS2R_V:
  case RVTrim::VS4R_V:
  case RVTrim::VS8R_V:
    if (!MI.getOperand(1).isFI())
      return Register();
    FrameIndex = MI.getOperand(1).getIndex();
    unsigned LMUL = *getLMULForRVVWholeLoadStore(MI.getOpcode());
    MemBytes = TypeSize::getScalable(RISCV::RVVBytesPerBlock * LMUL);
    return MI.getOperand(0).getReg();
  }

  if (MI.getOperand(1).isFI() && MI.getOperand(2).isImm() &&
      MI.getOperand(2).getImm() == 0) {
    FrameIndex = MI.getOperand(1).getIndex();
    return MI.getOperand(0).getReg();
  }

  return 0;
}

bool RVTrimInstrInfo::isReMaterializableImpl(
    const MachineInstr &MI) const {
  switch (RVTrim::getRVVMCOpcode(MI.getOpcode())) {
  case RVTrim::VMV_V_X:
  case RVTrim::VFMV_V_F:
  case RVTrim::VMV_V_I:
  case RVTrim::VMV_S_X:
  case RVTrim::VFMV_S_F:
  case RVTrim::VID_V:
    return MI.getOperand(1).isUndef();
  default:
    return TargetInstrInfo::isReMaterializableImpl(MI);
  }
}

static bool forwardCopyWillClobberTuple(unsigned DstReg, unsigned SrcReg,
                                        unsigned NumRegs) {
  return DstReg > SrcReg && (DstReg - SrcReg) < NumRegs;
}

static bool isConvertibleToVMV_V_V(const RVTrimSubtarget &STI,
                                   const MachineBasicBlock &MBB,
                                   MachineBasicBlock::const_iterator MBBI,
                                   MachineBasicBlock::const_iterator &DefMBBI,
                                   RISCVVType::VLMUL LMul) {
  if (STI.getCLOpts().prefer_whole_register_move)
    return false;

  assert(MBBI->getOpcode() == TargetOpcode::COPY &&
         "Unexpected COPY instruction.");
  Register SrcReg = MBBI->getOperand(1).getReg();
  const TargetRegisterInfo *TRI = STI.getRegisterInfo();

  bool FoundDef = false;
  bool FirstVSetVLI = false;
  unsigned FirstSEW = 0;
  while (MBBI != MBB.begin()) {
    --MBBI;
    if (MBBI->isMetaInstruction())
      continue;

    if (RVTrimInstrInfo::isVectorConfigInstr(*MBBI)) {
      // There is a vsetvli between COPY and source define instruction.
      // vy = def_vop ...  (producing instruction)
      // ...
      // vsetvli
      // ...
      // vx = COPY vy
      if (!FoundDef) {
        if (!FirstVSetVLI) {
          FirstVSetVLI = true;
          unsigned FirstVType = MBBI->getOperand(2).getImm();
          RISCVVType::VLMUL FirstLMul = RISCVVType::getVLMUL(FirstVType);
          FirstSEW = RISCVVType::getSEW(FirstVType);
          // The first encountered vsetvli must have the same lmul as the
          // register class of COPY.
          if (FirstLMul != LMul)
            return false;
        }
        // Only permit `vsetvli x0, x0, vtype` between COPY and the source
        // define instruction.
        if (!RVTrimInstrInfo::isVLPreservingConfig(*MBBI))
          return false;
        continue;
      }

      // MBBI is the first vsetvli before the producing instruction.
      unsigned VType = MBBI->getOperand(2).getImm();
      // If there is a vsetvli between COPY and the producing instruction.
      if (FirstVSetVLI) {
        // If SEW is different, return false.
        if (RISCVVType::getSEW(VType) != FirstSEW)
          return false;
      }

      // If the vsetvli is tail undisturbed, keep the whole register move.
      if (!RISCVVType::isTailAgnostic(VType))
        return false;

      // The checking is conservative. We only have register classes for
      // LMUL = 1/2/4/8. We should be able to convert vmv1r.v to vmv.v.v
      // for fractional LMUL operations. However, we could not use the vsetvli
      // lmul for widening operations. The result of widening operation is
      // 2 x LMUL.
      return LMul == RISCVVType::getVLMUL(VType);
    } else if (MBBI->isInlineAsm() || MBBI->isCall()) {
      return false;
    } else if (MBBI->getNumDefs()) {
      // Check all the instructions which will change VL.
      // For example, vleff has implicit def VL.
      if (MBBI->modifiesRegister(RVTrim::VL, /*TRI=*/nullptr))
        return false;

      // Only converting whole register copies to vmv.v.v when the defining
      // value appears in the explicit operands.
      for (const MachineOperand &MO : MBBI->explicit_operands()) {
        if (!MO.isReg() || !MO.isDef())
          continue;
        if (!FoundDef && TRI->regsOverlap(MO.getReg(), SrcReg)) {
          // We only permit the source of COPY has the same LMUL as the defined
          // operand.
          // There are cases we need to keep the whole register copy if the LMUL
          // is different.
          // For example,
          // $x0 = PseudoVSETIVLI 4, 73   // vsetivli zero, 4, e16,m2,ta,m
          // $v28m4 = PseudoVWADD_VV_M2 $v26m2, $v8m2
          // # The COPY may be created by vlmul_trunc intrinsic.
          // $v26m2 = COPY renamable $v28m2, implicit killed $v28m4
          //
          // After widening, the valid value will be 4 x e32 elements. If we
          // convert the COPY to vmv.v.v, it will only copy 4 x e16 elements.
          // FIXME: The COPY of subregister of Zvlsseg register will not be able
          // to convert to vmv.v.[v|i] under the constraint.
          if (MO.getReg() != SrcReg)
            return false;

          // In widening reduction instructions with LMUL_1 input vector case,
          // only checking the LMUL is insufficient due to reduction result is
          // always LMUL_1.
          // For example,
          // $x11 = PseudoVSETIVLI 1, 64 // vsetivli a1, 1, e8, m1, ta, mu
          // $v8m1 = PseudoVWREDSUM_VS_M1 $v26, $v27
          // $v26 = COPY killed renamable $v8
          // After widening, The valid value will be 1 x e16 elements. If we
          // convert the COPY to vmv.v.v, it will only copy 1 x e8 elements.
          uint64_t TSFlags = MBBI->getDesc().TSFlags;
          if (RVTrimII::isRVVWideningReduction(TSFlags))
            return false;

          // If the producing instruction does not depend on vsetvli, do not
          // convert COPY to vmv.v.v. For example, VL1R_V or PseudoVRELOAD.
          if (!RVTrimII::hasSEWOp(TSFlags) || !RVTrimII::hasVLOp(TSFlags))
            return false;

          // Found the definition.
          FoundDef = true;
          DefMBBI = MBBI;
          break;
        }
      }
    }
  }

  return false;
}

void RVTrimInstrInfo::copyPhysRegVector(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MBBI,
    const DebugLoc &DL, MCRegister DstReg, MCRegister SrcReg, bool KillSrc,
    const TargetRegisterClass *RegClass) const {
  const RVTrimRegisterInfo *TRI = STI.getRegisterInfo();
  RISCVVType::VLMUL LMul = RVTrimRI::getLMul(RegClass->TSFlags);
  unsigned NF = RVTrimRI::getNF(RegClass->TSFlags);

  uint16_t SrcEncoding = TRI->getEncodingValue(SrcReg);
  uint16_t DstEncoding = TRI->getEncodingValue(DstReg);
  auto [LMulVal, Fractional] = RISCVVType::decodeVLMUL(LMul);
  assert(!Fractional && "It is impossible be fractional lmul here.");
  unsigned NumRegs = NF * LMulVal;
  bool ReversedCopy =
      forwardCopyWillClobberTuple(DstEncoding, SrcEncoding, NumRegs);
  if (ReversedCopy) {
    // If the src and dest overlap when copying a tuple, we need to copy the
    // registers in reverse.
    SrcEncoding += NumRegs - 1;
    DstEncoding += NumRegs - 1;
  }

  unsigned I = 0;
  auto GetCopyInfo = [&](uint16_t SrcEncoding, uint16_t DstEncoding)
      -> std::tuple<RISCVVType::VLMUL, const TargetRegisterClass &, unsigned,
                    unsigned, unsigned> {
    if (ReversedCopy) {
      // For reversed copying, if there are enough aligned registers(8/4/2), we
      // can do a larger copy(LMUL8/4/2).
      // Besides, we have already known that DstEncoding is larger than
      // SrcEncoding in forwardCopyWillClobberTuple, so the difference between
      // DstEncoding and SrcEncoding should be >= LMUL value we try to use to
      // avoid clobbering.
      uint16_t Diff = DstEncoding - SrcEncoding;
      if (I + 8 <= NumRegs && Diff >= 8 && SrcEncoding % 8 == 7 &&
          DstEncoding % 8 == 7)
        return {RISCVVType::LMUL_8, RVTrim::VRM8RegClass, RVTrim::VMV8R_V,
                RVTrim::PseudoVMV_V_V_M8, RVTrim::PseudoVMV_V_I_M8};
      if (I + 4 <= NumRegs && Diff >= 4 && SrcEncoding % 4 == 3 &&
          DstEncoding % 4 == 3)
        return {RISCVVType::LMUL_4, RVTrim::VRM4RegClass, RVTrim::VMV4R_V,
                RVTrim::PseudoVMV_V_V_M4, RVTrim::PseudoVMV_V_I_M4};
      if (I + 2 <= NumRegs && Diff >= 2 && SrcEncoding % 2 == 1 &&
          DstEncoding % 2 == 1)
        return {RISCVVType::LMUL_2, RVTrim::VRM2RegClass, RVTrim::VMV2R_V,
                RVTrim::PseudoVMV_V_V_M2, RVTrim::PseudoVMV_V_I_M2};
      // Or we should do LMUL1 copying.
      return {RISCVVType::LMUL_1, RVTrim::VRRegClass, RVTrim::VMV1R_V,
              RVTrim::PseudoVMV_V_V_M1, RVTrim::PseudoVMV_V_I_M1};
    }

    // For forward copying, if source register encoding and destination register
    // encoding are aligned to 8/4/2, we can do a LMUL8/4/2 copying.
    if (I + 8 <= NumRegs && SrcEncoding % 8 == 0 && DstEncoding % 8 == 0)
      return {RISCVVType::LMUL_8, RVTrim::VRM8RegClass, RVTrim::VMV8R_V,
              RVTrim::PseudoVMV_V_V_M8, RVTrim::PseudoVMV_V_I_M8};
    if (I + 4 <= NumRegs && SrcEncoding % 4 == 0 && DstEncoding % 4 == 0)
      return {RISCVVType::LMUL_4, RVTrim::VRM4RegClass, RVTrim::VMV4R_V,
              RVTrim::PseudoVMV_V_V_M4, RVTrim::PseudoVMV_V_I_M4};
    if (I + 2 <= NumRegs && SrcEncoding % 2 == 0 && DstEncoding % 2 == 0)
      return {RISCVVType::LMUL_2, RVTrim::VRM2RegClass, RVTrim::VMV2R_V,
              RVTrim::PseudoVMV_V_V_M2, RVTrim::PseudoVMV_V_I_M2};
    // Or we should do LMUL1 copying.
    return {RISCVVType::LMUL_1, RVTrim::VRRegClass, RVTrim::VMV1R_V,
            RVTrim::PseudoVMV_V_V_M1, RVTrim::PseudoVMV_V_I_M1};
  };

  while (I != NumRegs) {
    // For non-segment copying, we only do this once as the registers are always
    // aligned.
    // For segment copying, we may do this several times. If the registers are
    // aligned to larger LMUL, we can eliminate some copyings.
    auto [LMulCopied, RegClass, Opc, VVOpc, VIOpc] =
        GetCopyInfo(SrcEncoding, DstEncoding);
    auto [NumCopied, _] = RISCVVType::decodeVLMUL(LMulCopied);

    MachineBasicBlock::const_iterator DefMBBI;
    if (LMul == LMulCopied &&
        isConvertibleToVMV_V_V(STI, MBB, MBBI, DefMBBI, LMul)) {
      Opc = VVOpc;
      if (DefMBBI->getOpcode() == VIOpc)
        Opc = VIOpc;
    }

    // Emit actual copying.
    // For reversed copying, the encoding should be decreased.
    MCRegister ActualSrcReg = TRI->findVRegWithEncoding(
        RegClass, ReversedCopy ? (SrcEncoding - NumCopied + 1) : SrcEncoding);
    MCRegister ActualDstReg = TRI->findVRegWithEncoding(
        RegClass, ReversedCopy ? (DstEncoding - NumCopied + 1) : DstEncoding);

    auto MIB = BuildMI(MBB, MBBI, DL, get(Opc), ActualDstReg);
    bool UseVMV_V_I = RVTrim::getRVVMCOpcode(Opc) == RVTrim::VMV_V_I;
    bool UseVMV = UseVMV_V_I || RVTrim::getRVVMCOpcode(Opc) == RVTrim::VMV_V_V;
    if (UseVMV)
      MIB.addReg(ActualDstReg, RegState::Undef);
    if (UseVMV_V_I)
      MIB = MIB.add(DefMBBI->getOperand(2));
    else
      MIB = MIB.addReg(ActualSrcReg, getKillRegState(KillSrc));
    if (UseVMV) {
      const MCInstrDesc &Desc = DefMBBI->getDesc();
      MIB.add(DefMBBI->getOperand(RVTrimII::getVLOpNum(Desc)));  // AVL
      unsigned Log2SEW =
          DefMBBI->getOperand(RVTrimII::getSEWOpNum(Desc)).getImm();
      MIB.addImm(Log2SEW ? Log2SEW : 3);                        // SEW
      MIB.addImm(0);                                            // tu, mu
      MIB.addReg(RVTrim::VL, RegState::Implicit);
      MIB.addReg(RVTrim::VTYPE, RegState::Implicit);
    }
    // Add an implicit read of the original source to silence the verifier
    // in the cases where some of the smaller VRs we're copying from might be
    // undef, caused by the fact that the original, larger source VR might not
    // be fully initialized at the time this COPY happens.
    MIB.addReg(SrcReg, RegState::Implicit);

    // If we are copying reversely, we should decrease the encoding.
    SrcEncoding += (ReversedCopy ? -NumCopied : NumCopied);
    DstEncoding += (ReversedCopy ? -NumCopied : NumCopied);
    I += NumCopied;
  }
}

void RVTrimInstrInfo::copyPhysReg(MachineBasicBlock &MBB,
                                 MachineBasicBlock::iterator MBBI,
                                 const DebugLoc &DL, Register DstReg,
                                 Register SrcReg, bool KillSrc,
                                 bool RenamableDest, bool RenamableSrc) const {
  const RVTrimRegisterInfo *TRI = STI.getRegisterInfo();
  RegState KillFlag = getKillRegState(KillSrc);

  if (RVTrim::GPRRegClass.contains(DstReg, SrcReg)) {
    BuildMI(MBB, MBBI, DL, get(RVTrim::ADDI), DstReg)
        .addReg(SrcReg, KillFlag | getRenamableRegState(RenamableSrc))
        .addImm(0);
    return;
  }

  if (RVTrim::GPRF16RegClass.contains(DstReg, SrcReg)) {
    BuildMI(MBB, MBBI, DL, get(RVTrim::PseudoMV_FPR16INX), DstReg)
        .addReg(SrcReg, KillFlag | getRenamableRegState(RenamableSrc));
    return;
  }

  if (RVTrim::GPRF32RegClass.contains(DstReg, SrcReg)) {
    BuildMI(MBB, MBBI, DL, get(RVTrim::PseudoMV_FPR32INX), DstReg)
        .addReg(SrcReg, KillFlag | getRenamableRegState(RenamableSrc));
    return;
  }

  if (RVTrim::GPRPairRegClass.contains(DstReg, SrcReg)) {
    if (!STI.is64Bit()) {
      if (STI.hasStdExtZdinx()) {
        // On RV32_Zdinx, FMV.D will move a pair of registers to another pair of
        // registers, in one instruction.
        BuildMI(MBB, MBBI, DL, get(RVTrim::FSGNJ_D_IN32X), DstReg)
            .addReg(SrcReg, getRenamableRegState(RenamableSrc))
            .addReg(SrcReg, KillFlag | getRenamableRegState(RenamableSrc));
        return;
      }

      if (STI.hasStdExtP()) {
        // On RV32P, `padd.dw` is a GPR Pair Add
        BuildMI(MBB, MBBI, DL, get(RVTrim::PADD_DW), DstReg)
            .addReg(RVTrim::X0_Pair)
            .addReg(SrcReg, KillFlag | getRenamableRegState(RenamableSrc));
        return;
      }
    }

    auto [EvenReg, OddReg] = TRI->getGPRPairSubRegs(SrcReg);
    assert(DstReg != RVTrim::X0_Pair && "Cannot write to X0_Pair");

    // Emit an ADDI for both parts of GPRPair.
    BuildMI(MBB, MBBI, DL, get(RVTrim::ADDI),
            TRI->getSubReg(DstReg, RVTrim::sub_gpr_even))
        .addReg(EvenReg, KillFlag)
        .addImm(0);
    BuildMI(MBB, MBBI, DL, get(RVTrim::ADDI),
            TRI->getSubReg(DstReg, RVTrim::sub_gpr_odd))
        .addReg(OddReg, KillFlag)
        .addImm(0);
    return;
  }

  // Handle copy from csr
  if (RVTrim::VCSRRegClass.contains(SrcReg) &&
      RVTrim::GPRRegClass.contains(DstReg)) {
    BuildMI(MBB, MBBI, DL, get(RVTrim::CSRRS), DstReg)
        .addImm(RVTrimSysReg::lookupSysRegByName(TRI->getName(SrcReg))->Encoding)
        .addReg(RVTrim::X0);
    return;
  }

  if (RVTrim::FPR16RegClass.contains(DstReg, SrcReg)) {
    unsigned Opc;
    if (STI.hasStdExtZfh()) {
      Opc = RVTrim::FSGNJ_H;
    } else {
      assert(STI.hasStdExtF() &&
             (STI.hasStdExtZfhmin() || STI.hasStdExtZfbfmin()) &&
             "Unexpected extensions");
      // Zfhmin/Zfbfmin doesn't have FSGNJ_H, replace FSGNJ_H with FSGNJ_S.
      DstReg = TRI->getMatchingSuperReg(DstReg, RVTrim::sub_16,
                                        &RVTrim::FPR32RegClass);
      SrcReg = TRI->getMatchingSuperReg(SrcReg, RVTrim::sub_16,
                                        &RVTrim::FPR32RegClass);
      Opc = RVTrim::FSGNJ_S;
    }
    BuildMI(MBB, MBBI, DL, get(Opc), DstReg)
        .addReg(SrcReg, KillFlag)
        .addReg(SrcReg, KillFlag);
    return;
  }

  if (RVTrim::FPR32RegClass.contains(DstReg, SrcReg)) {
    BuildMI(MBB, MBBI, DL, get(RVTrim::FSGNJ_S), DstReg)
        .addReg(SrcReg, KillFlag)
        .addReg(SrcReg, KillFlag);
    return;
  }

  if (RVTrim::FPR64RegClass.contains(DstReg, SrcReg)) {
    BuildMI(MBB, MBBI, DL, get(RVTrim::FSGNJ_D), DstReg)
        .addReg(SrcReg, KillFlag)
        .addReg(SrcReg, KillFlag);
    return;
  }

  if (RVTrim::FPR16RegClass.contains(DstReg) &&
      RVTrim::GPRRegClass.contains(SrcReg)) {
    assert(STI.hasHalfFPLoadStoreMove() && "Unexpected extensions");
    BuildMI(MBB, MBBI, DL, get(RVTrim::FMV_H_X), DstReg)
        .addReg(SrcReg, KillFlag);
    return;
  }

  if (RVTrim::GPRRegClass.contains(DstReg) &&
      RVTrim::FPR16RegClass.contains(SrcReg)) {
    assert(STI.hasHalfFPLoadStoreMove() && "Unexpected extensions");
    BuildMI(MBB, MBBI, DL, get(RVTrim::FMV_X_H), DstReg)
        .addReg(SrcReg, KillFlag);
    return;
  }

  if (RVTrim::FPR32RegClass.contains(DstReg) &&
      RVTrim::GPRRegClass.contains(SrcReg)) {
    BuildMI(MBB, MBBI, DL, get(RVTrim::FMV_W_X), DstReg)
        .addReg(SrcReg, KillFlag);
    return;
  }

  if (RVTrim::GPRRegClass.contains(DstReg) &&
      RVTrim::FPR32RegClass.contains(SrcReg)) {
    BuildMI(MBB, MBBI, DL, get(RVTrim::FMV_X_W), DstReg)
        .addReg(SrcReg, KillFlag);
    return;
  }

  if (RVTrim::FPR64RegClass.contains(DstReg) &&
      RVTrim::GPRRegClass.contains(SrcReg)) {
    assert(STI.getXLen() == 64 && "Unexpected GPR size");
    BuildMI(MBB, MBBI, DL, get(RVTrim::FMV_D_X), DstReg)
        .addReg(SrcReg, KillFlag);
    return;
  }

  if (RVTrim::GPRRegClass.contains(DstReg) &&
      RVTrim::FPR64RegClass.contains(SrcReg)) {
    assert(STI.getXLen() == 64 && "Unexpected GPR size");
    BuildMI(MBB, MBBI, DL, get(RVTrim::FMV_X_D), DstReg)
        .addReg(SrcReg, KillFlag);
    return;
  }

  // VR->VR copies.
  const TargetRegisterClass *RegClass =
      TRI->getCommonMinimalPhysRegClass(SrcReg, DstReg);
  if (RVTrimRegisterInfo::isRVVRegClass(RegClass)) {
    copyPhysRegVector(MBB, MBBI, DL, DstReg, SrcReg, KillSrc, RegClass);
    return;
  }

  llvm_unreachable("Impossible reg-to-reg copy");
}

void RVTrimInstrInfo::storeRegToStackSlot(MachineBasicBlock &MBB,
                                         MachineBasicBlock::iterator I,
                                         Register SrcReg, bool IsKill, int FI,
                                         const TargetRegisterClass *RC,
                                         Register VReg,
                                         MachineInstr::MIFlag Flags) const {
  MachineFunction *MF = MBB.getParent();
  MachineFrameInfo &MFI = MF->getFrameInfo();
  Align Alignment = MFI.getObjectAlign(FI);

  unsigned Opcode;
  if (RVTrim::GPRRegClass.hasSubClassEq(RC)) {
    Opcode = RegInfo.getRegSizeInBits(RVTrim::GPRRegClass) == 32 ? RVTrim::SW
                                                                : RVTrim::SD;
  } else if (RVTrim::GPRF16RegClass.hasSubClassEq(RC)) {
    Opcode = RVTrim::SH_INX;
  } else if (RVTrim::GPRF32RegClass.hasSubClassEq(RC)) {
    Opcode = RVTrim::SW_INX;
  } else if (RVTrim::GPRPairRegClass.hasSubClassEq(RC)) {
    if (!STI.is64Bit() && STI.hasStdExtZilsd() &&
        Alignment >= STI.getZilsdAlign()) {
      Opcode = RVTrim::SD_RV32;
    } else {
      Opcode = RVTrim::PseudoRV32ZdinxSD;
    }
  } else if (RVTrim::FPR16RegClass.hasSubClassEq(RC)) {
    Opcode = RVTrim::FSH;
  } else if (RVTrim::FPR32RegClass.hasSubClassEq(RC)) {
    Opcode = RVTrim::FSW;
  } else if (RVTrim::FPR64RegClass.hasSubClassEq(RC)) {
    Opcode = RVTrim::FSD;
  } else if (RVTrim::VRRegClass.hasSubClassEq(RC)) {
    Opcode = RVTrim::VS1R_V;
  } else if (RVTrim::VRM2RegClass.hasSubClassEq(RC)) {
    Opcode = RVTrim::VS2R_V;
  } else if (RVTrim::VRM4RegClass.hasSubClassEq(RC)) {
    Opcode = RVTrim::VS4R_V;
  } else if (RVTrim::VRM8RegClass.hasSubClassEq(RC)) {
    Opcode = RVTrim::VS8R_V;
  } else if (RVTrim::VRN2M1RegClass.hasSubClassEq(RC))
    Opcode = RVTrim::PseudoVSPILL2_M1;
  else if (RVTrim::VRN2M2RegClass.hasSubClassEq(RC))
    Opcode = RVTrim::PseudoVSPILL2_M2;
  else if (RVTrim::VRN2M4RegClass.hasSubClassEq(RC))
    Opcode = RVTrim::PseudoVSPILL2_M4;
  else if (RVTrim::VRN3M1RegClass.hasSubClassEq(RC))
    Opcode = RVTrim::PseudoVSPILL3_M1;
  else if (RVTrim::VRN3M2RegClass.hasSubClassEq(RC))
    Opcode = RVTrim::PseudoVSPILL3_M2;
  else if (RVTrim::VRN4M1RegClass.hasSubClassEq(RC))
    Opcode = RVTrim::PseudoVSPILL4_M1;
  else if (RVTrim::VRN4M2RegClass.hasSubClassEq(RC))
    Opcode = RVTrim::PseudoVSPILL4_M2;
  else if (RVTrim::VRN5M1RegClass.hasSubClassEq(RC))
    Opcode = RVTrim::PseudoVSPILL5_M1;
  else if (RVTrim::VRN6M1RegClass.hasSubClassEq(RC))
    Opcode = RVTrim::PseudoVSPILL6_M1;
  else if (RVTrim::VRN7M1RegClass.hasSubClassEq(RC))
    Opcode = RVTrim::PseudoVSPILL7_M1;
  else if (RVTrim::VRN8M1RegClass.hasSubClassEq(RC))
    Opcode = RVTrim::PseudoVSPILL8_M1;
  else
    llvm_unreachable("Can't store this register to stack slot");

  if (RVTrimRegisterInfo::isRVVRegClass(RC)) {
    MachineMemOperand *MMO = MF->getMachineMemOperand(
        MachinePointerInfo::getFixedStack(*MF, FI), MachineMemOperand::MOStore,
        TypeSize::getScalable(MFI.getObjectSize(FI)), Alignment);

    MFI.setStackID(FI, TargetStackID::ScalableVector);
    BuildMI(MBB, I, DebugLoc(), get(Opcode))
        .addReg(SrcReg, getKillRegState(IsKill))
        .addFrameIndex(FI)
        .addMemOperand(MMO)
        .setMIFlag(Flags);
    NumVRegSpilled += RegInfo.getRegSizeInBits(*RC) / RISCV::RVVBitsPerBlock;
  } else {
    MachineMemOperand *MMO = MF->getMachineMemOperand(
        MachinePointerInfo::getFixedStack(*MF, FI), MachineMemOperand::MOStore,
        MFI.getObjectSize(FI), Alignment);

    BuildMI(MBB, I, DebugLoc(), get(Opcode))
        .addReg(SrcReg, getKillRegState(IsKill))
        .addFrameIndex(FI)
        .addImm(0)
        .addMemOperand(MMO)
        .setMIFlag(Flags);
  }
}

void RVTrimInstrInfo::loadRegFromStackSlot(MachineBasicBlock &MBB,
                                          MachineBasicBlock::iterator I,
                                          Register DstReg, int FI,
                                          const TargetRegisterClass *RC,
                                          Register VReg, unsigned SubReg,
                                          MachineInstr::MIFlag Flags) const {
  MachineFunction *MF = MBB.getParent();
  MachineFrameInfo &MFI = MF->getFrameInfo();
  Align Alignment = MFI.getObjectAlign(FI);
  DebugLoc DL =
      Flags & MachineInstr::FrameDestroy ? MBB.findDebugLoc(I) : DebugLoc();

  unsigned Opcode;
  if (RVTrim::GPRRegClass.hasSubClassEq(RC)) {
    Opcode = RegInfo.getRegSizeInBits(RVTrim::GPRRegClass) == 32 ? RVTrim::LW
                                                                : RVTrim::LD;
  } else if (RVTrim::GPRF16RegClass.hasSubClassEq(RC)) {
    Opcode = RVTrim::LH_INX;
  } else if (RVTrim::GPRF32RegClass.hasSubClassEq(RC)) {
    Opcode = RVTrim::LW_INX;
  } else if (RVTrim::GPRPairRegClass.hasSubClassEq(RC)) {
    if (!STI.is64Bit() && STI.hasStdExtZilsd() &&
        Alignment >= STI.getZilsdAlign()) {
      Opcode = RVTrim::LD_RV32;
    } else {
      Opcode = RVTrim::PseudoRV32ZdinxLD;
    }
  } else if (RVTrim::FPR16RegClass.hasSubClassEq(RC)) {
    Opcode = RVTrim::FLH;
  } else if (RVTrim::FPR32RegClass.hasSubClassEq(RC)) {
    Opcode = RVTrim::FLW;
  } else if (RVTrim::FPR64RegClass.hasSubClassEq(RC)) {
    Opcode = RVTrim::FLD;
  } else if (RVTrim::VRRegClass.hasSubClassEq(RC)) {
    Opcode = RVTrim::VL1RE8_V;
  } else if (RVTrim::VRM2RegClass.hasSubClassEq(RC)) {
    Opcode = RVTrim::VL2RE8_V;
  } else if (RVTrim::VRM4RegClass.hasSubClassEq(RC)) {
    Opcode = RVTrim::VL4RE8_V;
  } else if (RVTrim::VRM8RegClass.hasSubClassEq(RC)) {
    Opcode = RVTrim::VL8RE8_V;
  } else if (RVTrim::VRN2M1RegClass.hasSubClassEq(RC))
    Opcode = RVTrim::PseudoVRELOAD2_M1;
  else if (RVTrim::VRN2M2RegClass.hasSubClassEq(RC))
    Opcode = RVTrim::PseudoVRELOAD2_M2;
  else if (RVTrim::VRN2M4RegClass.hasSubClassEq(RC))
    Opcode = RVTrim::PseudoVRELOAD2_M4;
  else if (RVTrim::VRN3M1RegClass.hasSubClassEq(RC))
    Opcode = RVTrim::PseudoVRELOAD3_M1;
  else if (RVTrim::VRN3M2RegClass.hasSubClassEq(RC))
    Opcode = RVTrim::PseudoVRELOAD3_M2;
  else if (RVTrim::VRN4M1RegClass.hasSubClassEq(RC))
    Opcode = RVTrim::PseudoVRELOAD4_M1;
  else if (RVTrim::VRN4M2RegClass.hasSubClassEq(RC))
    Opcode = RVTrim::PseudoVRELOAD4_M2;
  else if (RVTrim::VRN5M1RegClass.hasSubClassEq(RC))
    Opcode = RVTrim::PseudoVRELOAD5_M1;
  else if (RVTrim::VRN6M1RegClass.hasSubClassEq(RC))
    Opcode = RVTrim::PseudoVRELOAD6_M1;
  else if (RVTrim::VRN7M1RegClass.hasSubClassEq(RC))
    Opcode = RVTrim::PseudoVRELOAD7_M1;
  else if (RVTrim::VRN8M1RegClass.hasSubClassEq(RC))
    Opcode = RVTrim::PseudoVRELOAD8_M1;
  else
    llvm_unreachable("Can't load this register from stack slot");

  if (RVTrimRegisterInfo::isRVVRegClass(RC)) {
    MachineMemOperand *MMO = MF->getMachineMemOperand(
        MachinePointerInfo::getFixedStack(*MF, FI), MachineMemOperand::MOLoad,
        TypeSize::getScalable(MFI.getObjectSize(FI)), Alignment);

    MFI.setStackID(FI, TargetStackID::ScalableVector);
    BuildMI(MBB, I, DL, get(Opcode), DstReg)
        .addFrameIndex(FI)
        .addMemOperand(MMO)
        .setMIFlag(Flags);
    NumVRegReloaded += RegInfo.getRegSizeInBits(*RC) / RISCV::RVVBitsPerBlock;
  } else {
    MachineMemOperand *MMO = MF->getMachineMemOperand(
        MachinePointerInfo::getFixedStack(*MF, FI), MachineMemOperand::MOLoad,
        MFI.getObjectSize(FI), Alignment);

    BuildMI(MBB, I, DL, get(Opcode), DstReg)
        .addFrameIndex(FI)
        .addImm(0)
        .addMemOperand(MMO)
        .setMIFlag(Flags);
  }
}
std::optional<unsigned> getFoldedOpcode(MachineFunction &MF, MachineInstr &MI,
                                        ArrayRef<unsigned> Ops,
                                        const RVTrimSubtarget &ST) {

  // The below optimizations narrow the load so they are only valid for little
  // endian.
  // TODO: Support big endian by adding an offset into the frame object?
  if (MF.getDataLayout().isBigEndian())
    return std::nullopt;

  // Fold load from stack followed by sext.b/sext.h/sext.w/zext.b/zext.h/zext.w.
  if (Ops.size() != 1 || Ops[0] != 1)
    return std::nullopt;

  switch (MI.getOpcode()) {
  default:
    if (RVTrimInstrInfo::isSEXT_W(MI))
      return RVTrim::LW;
    if (RVTrimInstrInfo::isZEXT_W(MI))
      return RVTrim::LWU;
    if (RVTrimInstrInfo::isZEXT_B(MI))
      return RVTrim::LBU;
    break;
  case RVTrim::SEXT_H:
    return RVTrim::LH;
  case RVTrim::SEXT_B:
    return RVTrim::LB;
  case RVTrim::ZEXT_H_RV32:
  case RVTrim::ZEXT_H_RV64:
    return RVTrim::LHU;
  }

  switch (RVTrim::getRVVMCOpcode(MI.getOpcode())) {
  default:
    return std::nullopt;
  case RVTrim::VMV_X_S: {
    unsigned Log2SEW =
        MI.getOperand(RVTrimII::getSEWOpNum(MI.getDesc())).getImm();
    if (ST.getXLen() < (1U << Log2SEW))
      return std::nullopt;
    switch (Log2SEW) {
    case 3:
      return RVTrim::LB;
    case 4:
      return RVTrim::LH;
    case 5:
      return RVTrim::LW;
    case 6:
      return RVTrim::LD;
    default:
      llvm_unreachable("Unexpected SEW");
    }
  }
  case RVTrim::VFMV_F_S: {
    unsigned Log2SEW =
        MI.getOperand(RVTrimII::getSEWOpNum(MI.getDesc())).getImm();
    switch (Log2SEW) {
    case 4:
      return RVTrim::FLH;
    case 5:
      return RVTrim::FLW;
    case 6:
      return RVTrim::FLD;
    default:
      llvm_unreachable("Unexpected SEW");
    }
  }
  }
}

// This is the version used during InlineSpiller::spillAroundUses
MachineInstr *
RVTrimInstrInfo::foldMemoryOperandImpl(MachineFunction &MF, MachineInstr &MI,
                                      ArrayRef<unsigned> Ops, int FrameIndex,
                                      MachineInstr *&CopyMI, LiveIntervals *LIS,
                                      VirtRegMap *VRM) const {
  MachineBasicBlock::iterator InsertPt = MI;
  std::optional<unsigned> LoadOpc = getFoldedOpcode(MF, MI, Ops, STI);
  if (!LoadOpc)
    return nullptr;
  Register DstReg = MI.getOperand(0).getReg();
  return BuildMI(*MI.getParent(), InsertPt, MI.getDebugLoc(), get(*LoadOpc),
                 DstReg)
      .addFrameIndex(FrameIndex)
      .addImm(0);
}

static unsigned getLoadPredicatedOpcode(unsigned Opcode) {
  switch (Opcode) {
  case RVTrim::LB:
    return RVTrim::PseudoCCLB;
  case RVTrim::LBU:
    return RVTrim::PseudoCCLBU;
  case RVTrim::LH:
    return RVTrim::PseudoCCLH;
  case RVTrim::LHU:
    return RVTrim::PseudoCCLHU;
  case RVTrim::LW:
    return RVTrim::PseudoCCLW;
  case RVTrim::LWU:
    return RVTrim::PseudoCCLWU;
  case RVTrim::LD:
    return RVTrim::PseudoCCLD;
  case RVTrim::QC_E_LB:
    return RVTrim::PseudoCCQC_E_LB;
  case RVTrim::QC_E_LBU:
    return RVTrim::PseudoCCQC_E_LBU;
  case RVTrim::QC_E_LH:
    return RVTrim::PseudoCCQC_E_LH;
  case RVTrim::QC_E_LHU:
    return RVTrim::PseudoCCQC_E_LHU;
  case RVTrim::QC_E_LW:
    return RVTrim::PseudoCCQC_E_LW;
  default:
    return 0;
  }
}

MachineInstr *RVTrimInstrInfo::foldMemoryOperandImpl(
    MachineFunction &MF, MachineInstr &MI, ArrayRef<unsigned> Ops,
    MachineInstr &LoadMI, MachineInstr *&CopyMI, LiveIntervals *LIS,
    VirtRegMap *VRM) const {
  MachineBasicBlock::iterator InsertPt = MI;
  // For now, only handle RVTrim::PseudoCCMOVGPR.
  if (MI.getOpcode() != RVTrim::PseudoCCMOVGPR)
    return nullptr;

  unsigned PredOpc = getLoadPredicatedOpcode(LoadMI.getOpcode());

  if (!STI.hasShortForwardBranchILoad() || !PredOpc)
    return nullptr;

  MachineRegisterInfo &MRI = MF.getRegInfo();
  if (Ops.size() != 1 || (Ops[0] != 1 && Ops[0] != 2))
    return nullptr;

  bool Invert = Ops[0] == 2;
  const MachineOperand &FalseReg = MI.getOperand(!Invert ? 2 : 1);
  Register DestReg = MI.getOperand(0).getReg();
  const TargetRegisterClass *PreviousClass = MRI.getRegClass(FalseReg.getReg());
  if (!MRI.constrainRegClass(DestReg, PreviousClass))
    return nullptr;

  // Create a new predicated version of DefMI.
  MachineInstrBuilder NewMI = BuildMI(*MI.getParent(), InsertPt,
                                      MI.getDebugLoc(), get(PredOpc), DestReg);

  // Copy the false register.
  NewMI.add(FalseReg);

  // Copy all the DefMI operands.
  const MCInstrDesc &DefDesc = LoadMI.getDesc();
  for (unsigned i = 1, e = DefDesc.getNumOperands(); i != e; ++i)
    NewMI.add(LoadMI.getOperand(i));

  // Add branch opcode, inverting if necessary.
  unsigned BCC = MI.getOperand(MI.getNumExplicitOperands() - 3).getImm();
  if (!Invert)
    BCC = RVTrimCC::getInverseBranchOpcode(BCC);
  NewMI.addImm(BCC);

  // Copy condition portion
  NewMI.add({MI.getOperand(MI.getNumExplicitOperands() - 2),
             MI.getOperand(MI.getNumExplicitOperands() - 1)});
  NewMI.cloneMemRefs(LoadMI);
  return NewMI;
}

void RVTrimInstrInfo::movImm(MachineBasicBlock &MBB,
                            MachineBasicBlock::iterator MBBI,
                            const DebugLoc &DL, Register DstReg, uint64_t Val,
                            MachineInstr::MIFlag Flag, bool DstRenamable,
                            bool DstIsDead) const {
  Register SrcReg = RVTrim::X0;

  // For RV32, allow a sign or unsigned 32 bit value.
  if (!STI.is64Bit() && !isInt<32>(Val)) {
    // If have a uimm32 it will still fit in a register so we can allow it.
    if (!isUInt<32>(Val))
      report_fatal_error("Should only materialize 32-bit constants for RV32");

    // Sign extend for generateInstSeq.
    Val = SignExtend64<32>(Val);
  }

  RVTrimMatInt::InstSeq Seq = RVTrimMatInt::generateInstSeq(Val, STI);
  assert(!Seq.empty());

  bool SrcRenamable = false;
  unsigned Num = 0;

  for (const RVTrimMatInt::Inst &Inst : Seq) {
    bool LastItem = ++Num == Seq.size();
    RegState DstRegState = getDeadRegState(DstIsDead && LastItem) |
                           getRenamableRegState(DstRenamable);
    RegState SrcRegState = getKillRegState(SrcReg != RVTrim::X0) |
                           getRenamableRegState(SrcRenamable);
    switch (Inst.getOpndKind()) {
    case RVTrimMatInt::Imm:
      BuildMI(MBB, MBBI, DL, get(Inst.getOpcode()))
          .addDef(DstReg, DstRegState)
          .addImm(Inst.getImm())
          .setMIFlag(Flag);
      break;
    case RVTrimMatInt::RegX0:
      BuildMI(MBB, MBBI, DL, get(Inst.getOpcode()))
          .addDef(DstReg, DstRegState)
          .addReg(SrcReg, SrcRegState)
          .addReg(RVTrim::X0)
          .setMIFlag(Flag);
      break;
    case RVTrimMatInt::RegReg:
      BuildMI(MBB, MBBI, DL, get(Inst.getOpcode()))
          .addDef(DstReg, DstRegState)
          .addReg(SrcReg, SrcRegState)
          .addReg(SrcReg, SrcRegState)
          .setMIFlag(Flag);
      break;
    case RVTrimMatInt::RegImm:
      BuildMI(MBB, MBBI, DL, get(Inst.getOpcode()))
          .addDef(DstReg, DstRegState)
          .addReg(SrcReg, SrcRegState)
          .addImm(Inst.getImm())
          .setMIFlag(Flag);
      break;
    }

    // Only the first instruction has X0 as its source.
    SrcReg = DstReg;
    SrcRenamable = DstRenamable;
  }
}

RVTrimCC::CondCode RVTrimInstrInfo::getCondFromBranchOpc(unsigned Opc) {
  switch (Opc) {
  default:
    return RVTrimCC::COND_INVALID;
  case RVTrim::BEQ:
  case RVTrim::BEQI:
  case RVTrim::CV_BEQIMM:
  case RVTrim::QC_BEQI:
  case RVTrim::QC_E_BEQI:
  case RVTrim::NDS_BBC:
  case RVTrim::NDS_BEQC:
    return RVTrimCC::COND_EQ;
  case RVTrim::BNE:
  case RVTrim::BNEI:
  case RVTrim::QC_BNEI:
  case RVTrim::QC_E_BNEI:
  case RVTrim::CV_BNEIMM:
  case RVTrim::NDS_BBS:
  case RVTrim::NDS_BNEC:
    return RVTrimCC::COND_NE;
  case RVTrim::BLT:
  case RVTrim::QC_BLTI:
  case RVTrim::QC_E_BLTI:
    return RVTrimCC::COND_LT;
  case RVTrim::BGE:
  case RVTrim::QC_BGEI:
  case RVTrim::QC_E_BGEI:
    return RVTrimCC::COND_GE;
  case RVTrim::BLTU:
  case RVTrim::QC_BLTUI:
  case RVTrim::QC_E_BLTUI:
    return RVTrimCC::COND_LTU;
  case RVTrim::BGEU:
  case RVTrim::QC_BGEUI:
  case RVTrim::QC_E_BGEUI:
    return RVTrimCC::COND_GEU;
  }
}

bool RVTrimInstrInfo::evaluateCondBranch(RVTrimCC::CondCode CC, int64_t C0,
                                        int64_t C1) {
  switch (CC) {
  default:
    llvm_unreachable("Unexpected CC");
  case RVTrimCC::COND_EQ:
    return C0 == C1;
  case RVTrimCC::COND_NE:
    return C0 != C1;
  case RVTrimCC::COND_LT:
    return C0 < C1;
  case RVTrimCC::COND_GE:
    return C0 >= C1;
  case RVTrimCC::COND_LTU:
    return (uint64_t)C0 < (uint64_t)C1;
  case RVTrimCC::COND_GEU:
    return (uint64_t)C0 >= (uint64_t)C1;
  }
}

// The contents of values added to Cond are not examined outside of
// RVTrimInstrInfo, giving us flexibility in what to push to it. For RVTrim, we
// push BranchOpcode, Reg1, Reg2.
static void parseCondBranch(MachineInstr &LastInst, MachineBasicBlock *&Target,
                            SmallVectorImpl<MachineOperand> &Cond) {
  // Block ends with fall-through condbranch.
  assert(LastInst.getDesc().isConditionalBranch() &&
         "Unknown conditional branch");
  Target = LastInst.getOperand(2).getMBB();
  Cond.push_back(MachineOperand::CreateImm(LastInst.getOpcode()));
  Cond.push_back(LastInst.getOperand(0));
  Cond.push_back(LastInst.getOperand(1));
}

static unsigned getInverseXqcicmOpcode(unsigned Opcode) {
  switch (Opcode) {
  default:
    llvm_unreachable("Unexpected Opcode");
  case RVTrim::QC_MVEQ:
    return RVTrim::QC_MVNE;
  case RVTrim::QC_MVNE:
    return RVTrim::QC_MVEQ;
  case RVTrim::QC_MVLT:
    return RVTrim::QC_MVGE;
  case RVTrim::QC_MVGE:
    return RVTrim::QC_MVLT;
  case RVTrim::QC_MVLTU:
    return RVTrim::QC_MVGEU;
  case RVTrim::QC_MVGEU:
    return RVTrim::QC_MVLTU;
  case RVTrim::QC_MVEQI:
    return RVTrim::QC_MVNEI;
  case RVTrim::QC_MVNEI:
    return RVTrim::QC_MVEQI;
  case RVTrim::QC_MVLTI:
    return RVTrim::QC_MVGEI;
  case RVTrim::QC_MVGEI:
    return RVTrim::QC_MVLTI;
  case RVTrim::QC_MVLTUI:
    return RVTrim::QC_MVGEUI;
  case RVTrim::QC_MVGEUI:
    return RVTrim::QC_MVLTUI;
  }
}

unsigned RVTrimCC::getBrCond(RVTrimCC::CondCode CC, unsigned SelectOpc) {
  switch (SelectOpc) {
  default:
    switch (CC) {
    default:
      llvm_unreachable("Unexpected condition code!");
    case RVTrimCC::COND_EQ:
      return RVTrim::BEQ;
    case RVTrimCC::COND_NE:
      return RVTrim::BNE;
    case RVTrimCC::COND_LT:
      return RVTrim::BLT;
    case RVTrimCC::COND_GE:
      return RVTrim::BGE;
    case RVTrimCC::COND_LTU:
      return RVTrim::BLTU;
    case RVTrimCC::COND_GEU:
      return RVTrim::BGEU;
    }
    break;
  case RVTrim::Select_GPR_Using_CC_Imm5_Zibi:
    switch (CC) {
    default:
      llvm_unreachable("Unexpected condition code!");
    case RVTrimCC::COND_EQ:
      return RVTrim::BEQI;
    case RVTrimCC::COND_NE:
      return RVTrim::BNEI;
    }
    break;
  case RVTrim::Select_GPR_Using_CC_SImm5_CV:
    switch (CC) {
    default:
      llvm_unreachable("Unexpected condition code!");
    case RVTrimCC::COND_EQ:
      return RVTrim::CV_BEQIMM;
    case RVTrimCC::COND_NE:
      return RVTrim::CV_BNEIMM;
    }
    break;
  case RVTrim::Select_GPRNoX0_Using_CC_SImm5NonZero_QC:
    switch (CC) {
    default:
      llvm_unreachable("Unexpected condition code!");
    case RVTrimCC::COND_EQ:
      return RVTrim::QC_BEQI;
    case RVTrimCC::COND_NE:
      return RVTrim::QC_BNEI;
    case RVTrimCC::COND_LT:
      return RVTrim::QC_BLTI;
    case RVTrimCC::COND_GE:
      return RVTrim::QC_BGEI;
    }
    break;
  case RVTrim::Select_GPRNoX0_Using_CC_UImm5NonZero_QC:
    switch (CC) {
    default:
      llvm_unreachable("Unexpected condition code!");
    case RVTrimCC::COND_LTU:
      return RVTrim::QC_BLTUI;
    case RVTrimCC::COND_GEU:
      return RVTrim::QC_BGEUI;
    }
    break;
  case RVTrim::Select_GPRNoX0_Using_CC_SImm16NonZero_QC:
    switch (CC) {
    default:
      llvm_unreachable("Unexpected condition code!");
    case RVTrimCC::COND_EQ:
      return RVTrim::QC_E_BEQI;
    case RVTrimCC::COND_NE:
      return RVTrim::QC_E_BNEI;
    case RVTrimCC::COND_LT:
      return RVTrim::QC_E_BLTI;
    case RVTrimCC::COND_GE:
      return RVTrim::QC_E_BGEI;
    }
    break;
  case RVTrim::Select_GPRNoX0_Using_CC_UImm16NonZero_QC:
    switch (CC) {
    default:
      llvm_unreachable("Unexpected condition code!");
    case RVTrimCC::COND_LTU:
      return RVTrim::QC_E_BLTUI;
    case RVTrimCC::COND_GEU:
      return RVTrim::QC_E_BGEUI;
    }
    break;
  case RVTrim::Select_GPR_Using_CC_UImmLog2XLen_NDS:
    switch (CC) {
    default:
      llvm_unreachable("Unexpected condition code!");
    case RVTrimCC::COND_EQ:
      return RVTrim::NDS_BBC;
    case RVTrimCC::COND_NE:
      return RVTrim::NDS_BBS;
    }
    break;
  case RVTrim::Select_GPR_Using_CC_UImm7_NDS:
    switch (CC) {
    default:
      llvm_unreachable("Unexpected condition code!");
    case RVTrimCC::COND_EQ:
      return RVTrim::NDS_BEQC;
    case RVTrimCC::COND_NE:
      return RVTrim::NDS_BNEC;
    }
    break;
  }
}

RVTrimCC::CondCode RVTrimCC::getInverseBranchCondition(RVTrimCC::CondCode CC) {
  switch (CC) {
  default:
    llvm_unreachable("Unrecognized conditional branch");
  case RVTrimCC::COND_EQ:
    return RVTrimCC::COND_NE;
  case RVTrimCC::COND_NE:
    return RVTrimCC::COND_EQ;
  case RVTrimCC::COND_LT:
    return RVTrimCC::COND_GE;
  case RVTrimCC::COND_GE:
    return RVTrimCC::COND_LT;
  case RVTrimCC::COND_LTU:
    return RVTrimCC::COND_GEU;
  case RVTrimCC::COND_GEU:
    return RVTrimCC::COND_LTU;
  }
}

// Return inverse branch
unsigned RVTrimCC::getInverseBranchOpcode(unsigned BCC) {
  switch (BCC) {
  default:
    llvm_unreachable("Unexpected branch opcode!");
  case RVTrim::BEQ:
    return RVTrim::BNE;
  case RVTrim::BEQI:
    return RVTrim::BNEI;
  case RVTrim::BNE:
    return RVTrim::BEQ;
  case RVTrim::BNEI:
    return RVTrim::BEQI;
  case RVTrim::BLT:
    return RVTrim::BGE;
  case RVTrim::BGE:
    return RVTrim::BLT;
  case RVTrim::BLTU:
    return RVTrim::BGEU;
  case RVTrim::BGEU:
    return RVTrim::BLTU;
  case RVTrim::CV_BEQIMM:
    return RVTrim::CV_BNEIMM;
  case RVTrim::CV_BNEIMM:
    return RVTrim::CV_BEQIMM;
  case RVTrim::QC_BEQI:
    return RVTrim::QC_BNEI;
  case RVTrim::QC_BNEI:
    return RVTrim::QC_BEQI;
  case RVTrim::QC_BLTI:
    return RVTrim::QC_BGEI;
  case RVTrim::QC_BGEI:
    return RVTrim::QC_BLTI;
  case RVTrim::QC_BLTUI:
    return RVTrim::QC_BGEUI;
  case RVTrim::QC_BGEUI:
    return RVTrim::QC_BLTUI;
  case RVTrim::QC_E_BEQI:
    return RVTrim::QC_E_BNEI;
  case RVTrim::QC_E_BNEI:
    return RVTrim::QC_E_BEQI;
  case RVTrim::QC_E_BLTI:
    return RVTrim::QC_E_BGEI;
  case RVTrim::QC_E_BGEI:
    return RVTrim::QC_E_BLTI;
  case RVTrim::QC_E_BLTUI:
    return RVTrim::QC_E_BGEUI;
  case RVTrim::QC_E_BGEUI:
    return RVTrim::QC_E_BLTUI;
  case RVTrim::NDS_BBC:
    return RVTrim::NDS_BBS;
  case RVTrim::NDS_BBS:
    return RVTrim::NDS_BBC;
  case RVTrim::NDS_BEQC:
    return RVTrim::NDS_BNEC;
  case RVTrim::NDS_BNEC:
    return RVTrim::NDS_BEQC;
  }
}

bool RVTrimInstrInfo::analyzeBranch(MachineBasicBlock &MBB,
                                   MachineBasicBlock *&TBB,
                                   MachineBasicBlock *&FBB,
                                   SmallVectorImpl<MachineOperand> &Cond,
                                   bool AllowModify) const {
  TBB = FBB = nullptr;
  Cond.clear();

  // If the block has no terminators, it just falls into the block after it.
  MachineBasicBlock::iterator I = MBB.getLastNonDebugInstr();
  if (I == MBB.end() || !isUnpredicatedTerminator(*I))
    return false;

  // Count the number of terminators and find the first unconditional or
  // indirect branch.
  MachineBasicBlock::iterator FirstUncondOrIndirectBr = MBB.end();
  int NumTerminators = 0;
  for (auto J = I.getReverse(); J != MBB.rend() && isUnpredicatedTerminator(*J);
       J++) {
    NumTerminators++;
    if (J->getDesc().isUnconditionalBranch() ||
        J->getDesc().isIndirectBranch()) {
      FirstUncondOrIndirectBr = J.getReverse();
    }
  }

  // If AllowModify is true, we can erase any terminators after
  // FirstUncondOrIndirectBR.
  if (AllowModify && FirstUncondOrIndirectBr != MBB.end()) {
    while (std::next(FirstUncondOrIndirectBr) != MBB.end()) {
      std::next(FirstUncondOrIndirectBr)->eraseFromParent();
      NumTerminators--;
    }
    I = FirstUncondOrIndirectBr;
  }

  // We can't handle blocks that end in an indirect branch.
  if (I->getDesc().isIndirectBranch())
    return true;

  // We can't handle Generic branch opcodes from Global ISel.
  if (I->isPreISelOpcode())
    return true;

  // We can't handle blocks with more than 2 terminators.
  if (NumTerminators > 2)
    return true;

  // Handle a single unconditional branch.
  if (NumTerminators == 1 && I->getDesc().isUnconditionalBranch()) {
    TBB = getBranchDestBlock(*I);
    return false;
  }

  // Handle a single conditional branch.
  if (NumTerminators == 1 && I->getDesc().isConditionalBranch()) {
    parseCondBranch(*I, TBB, Cond);
    return false;
  }

  // Handle a conditional branch followed by an unconditional branch.
  if (NumTerminators == 2 && std::prev(I)->getDesc().isConditionalBranch() &&
      I->getDesc().isUnconditionalBranch()) {
    parseCondBranch(*std::prev(I), TBB, Cond);
    FBB = getBranchDestBlock(*I);
    return false;
  }

  // Otherwise, we can't handle this.
  return true;
}

unsigned RVTrimInstrInfo::removeBranch(MachineBasicBlock &MBB,
                                      int *BytesRemoved) const {
  if (BytesRemoved)
    *BytesRemoved = 0;
  MachineBasicBlock::iterator I = MBB.getLastNonDebugInstr();
  if (I == MBB.end())
    return 0;

  if (!I->getDesc().isUnconditionalBranch() &&
      !I->getDesc().isConditionalBranch())
    return 0;

  // Remove the branch.
  if (BytesRemoved)
    *BytesRemoved += getInstSizeInBytes(*I);
  I->eraseFromParent();

  I = MBB.end();

  if (I == MBB.begin())
    return 1;
  --I;
  if (!I->getDesc().isConditionalBranch())
    return 1;

  // Remove the branch.
  if (BytesRemoved)
    *BytesRemoved += getInstSizeInBytes(*I);
  I->eraseFromParent();
  return 2;
}

// Inserts a branch into the end of the specific MachineBasicBlock, returning
// the number of instructions inserted.
unsigned RVTrimInstrInfo::insertBranch(
    MachineBasicBlock &MBB, MachineBasicBlock *TBB, MachineBasicBlock *FBB,
    ArrayRef<MachineOperand> Cond, const DebugLoc &DL, int *BytesAdded) const {
  if (BytesAdded)
    *BytesAdded = 0;

  // Shouldn't be a fall through.
  assert(TBB && "insertBranch must not be told to insert a fallthrough");
  assert((Cond.size() == 3 || Cond.size() == 0) &&
         "RISC-V branch conditions have two components!");

  // Unconditional branch.
  if (Cond.empty()) {
    MachineInstr &MI = *BuildMI(&MBB, DL, get(RVTrim::PseudoBR)).addMBB(TBB);
    if (BytesAdded)
      *BytesAdded += getInstSizeInBytes(MI);
    return 1;
  }

  // Either a one or two-way conditional branch.
  MachineInstr &CondMI = *BuildMI(&MBB, DL, get(Cond[0].getImm()))
                              .add(Cond[1])
                              .add(Cond[2])
                              .addMBB(TBB);
  if (BytesAdded)
    *BytesAdded += getInstSizeInBytes(CondMI);

  // One-way conditional branch.
  if (!FBB)
    return 1;

  // Two-way conditional branch.
  MachineInstr &MI = *BuildMI(&MBB, DL, get(RVTrim::PseudoBR)).addMBB(FBB);
  if (BytesAdded)
    *BytesAdded += getInstSizeInBytes(MI);
  return 2;
}

void RVTrimInstrInfo::insertIndirectBranch(MachineBasicBlock &MBB,
                                          MachineBasicBlock &DestBB,
                                          MachineBasicBlock &RestoreBB,
                                          const DebugLoc &DL, int64_t BrOffset,
                                          RegScavenger *RS) const {
  assert(RS && "RegScavenger required for long branching");
  assert(MBB.empty() &&
         "new block should be inserted for expanding unconditional branch");
  assert(MBB.pred_size() == 1);
  assert(RestoreBB.empty() &&
         "restore block should be inserted for restoring clobbered registers");

  MachineFunction *MF = MBB.getParent();
  MachineRegisterInfo &MRI = MF->getRegInfo();
  RVTrimMachineFunctionInfo *RVFI = MF->getInfo<RVTrimMachineFunctionInfo>();
  const TargetRegisterInfo *TRI = MF->getSubtarget().getRegisterInfo();

  if (!isInt<32>(BrOffset))
    report_fatal_error(
        "Branch offsets outside of the signed 32-bit range not supported");

  // FIXME: A virtual register must be used initially, as the register
  // scavenger won't work with empty blocks (SIInstrInfo::insertIndirectBranch
  // uses the same workaround).
  Register ScratchReg = MRI.createVirtualRegister(&RVTrim::GPRJALRRegClass);
  auto II = MBB.end();
  // We may also update the jump target to RestoreBB later.
  MachineInstr &MI = *BuildMI(MBB, II, DL, get(RVTrim::PseudoJump))
                          .addDef(ScratchReg, RegState::Dead)
                          .addMBB(&DestBB, RVTrimII::MO_CALL);

  RS->enterBasicBlockEnd(MBB);
  // When cf-protection-branch is enabled, we must use t2 (x7) for software
  // guarded branches to hold the landing pad label.
  bool HasCFBranch =
      MF->getInfo<RVTrimMachineFunctionInfo>()->hasCFProtectionBranch();
  const TargetRegisterClass *RC = &RVTrim::GPRRegClass;
  if (HasCFBranch)
    RC = &RVTrim::GPRX7RegClass;
  Register TmpGPR =
      RS->scavengeRegisterBackwards(*RC, MI.getIterator(),
                                    /*RestoreAfter=*/false, /*SpAdj=*/0,
                                    /*AllowSpill=*/false);
  if (TmpGPR.isValid())
    RS->setRegUsed(TmpGPR);
  else {
    // The case when there is no scavenged register needs special handling.

    // Pick s11(or s1 for rve) because it doesn't make a difference.
    TmpGPR = STI.hasStdExtE() ? RVTrim::X9 : RVTrim::X27;
    // Force t2 if cf-protection-branch is enabled
    if (HasCFBranch)
      TmpGPR = RVTrim::X7;

    int FrameIndex = RVFI->getBranchRelaxationScratchFrameIndex();
    if (FrameIndex == -1)
      report_fatal_error("underestimated function size");

    storeRegToStackSlot(MBB, MI, TmpGPR, /*IsKill=*/true, FrameIndex,
                        &RVTrim::GPRRegClass, Register());
    TRI->eliminateFrameIndex(std::prev(MI.getIterator()),
                             /*SpAdj=*/0, /*FIOperandNum=*/1);

    MI.getOperand(1).setMBB(&RestoreBB);

    loadRegFromStackSlot(RestoreBB, RestoreBB.end(), TmpGPR, FrameIndex,
                         &RVTrim::GPRRegClass, Register());
    TRI->eliminateFrameIndex(RestoreBB.back(),
                             /*SpAdj=*/0, /*FIOperandNum=*/1);
  }

  MRI.replaceRegWith(ScratchReg, TmpGPR);
  MRI.clearVirtRegs();
}

bool RVTrimInstrInfo::reverseBranchCondition(
    SmallVectorImpl<MachineOperand> &Cond) const {
  assert((Cond.size() == 3) && "Invalid branch condition!");

  Cond[0].setImm(RVTrimCC::getInverseBranchOpcode(Cond[0].getImm()));

  return false;
}

// Return true if the instruction is a load immediate instruction (i.e.
// (ADDI x0, imm) or (BSETI x0, imm)).
static bool isLoadImm(const MachineInstr *MI, int64_t &Imm) {
  if (MI->getOpcode() == RVTrim::ADDI && MI->getOperand(1).isReg() &&
      MI->getOperand(1).getReg() == RVTrim::X0) {
    Imm = MI->getOperand(2).getImm();
    return true;
  }
  // BSETI can be used to create power of 2 constants. Only 2048 is currently
  // interesting because it is 1 more than the maximum ADDI constant.
  if (MI->getOpcode() == RVTrim::BSETI && MI->getOperand(1).isReg() &&
      MI->getOperand(1).getReg() == RVTrim::X0 &&
      MI->getOperand(2).getImm() == 11) {
    Imm = 2048;
    return true;
  }
  return false;
}

bool RVTrimInstrInfo::isFromLoadImm(const MachineRegisterInfo &MRI,
                                   const MachineOperand &Op, int64_t &Imm) {
  // Either a load from immediate instruction or X0.
  if (!Op.isReg())
    return false;

  Register Reg = Op.getReg();
  if (Reg == RVTrim::X0) {
    Imm = 0;
    return true;
  }

  if (!Reg.isVirtual())
    return false;

  const MachineInstr *DefMI = MRI.getVRegDef(Reg);
  return DefMI && isLoadImm(DefMI, Imm);
}

bool RVTrimInstrInfo::optimizeCondBranch(MachineInstr &MI) const {
  bool IsSigned = false;
  bool IsEquality = false;
  switch (MI.getOpcode()) {
  default:
    return false;
  case RVTrim::BEQ:
  case RVTrim::BNE:
    IsEquality = true;
    break;
  case RVTrim::BGE:
  case RVTrim::BLT:
    IsSigned = true;
    break;
  case RVTrim::BGEU:
  case RVTrim::BLTU:
    break;
  }

  MachineBasicBlock *MBB = MI.getParent();
  MachineRegisterInfo &MRI = MBB->getParent()->getRegInfo();

  const MachineOperand &LHS = MI.getOperand(0);
  const MachineOperand &RHS = MI.getOperand(1);
  MachineBasicBlock *TBB = MI.getOperand(2).getMBB();

  RVTrimCC::CondCode CC = getCondFromBranchOpc(MI.getOpcode());
  assert(CC != RVTrimCC::COND_INVALID);

  // Canonicalize conditional branches which can be constant folded into
  // beqz or bnez.  We can't modify the CFG here.
  int64_t C0, C1;
  if (isFromLoadImm(MRI, LHS, C0) && isFromLoadImm(MRI, RHS, C1)) {
    unsigned NewOpc = evaluateCondBranch(CC, C0, C1) ? RVTrim::BEQ : RVTrim::BNE;
    // Build the new branch and remove the old one.
    BuildMI(*MBB, MI, MI.getDebugLoc(), get(NewOpc))
        .addReg(RVTrim::X0)
        .addReg(RVTrim::X0)
        .addMBB(TBB);
    MI.eraseFromParent();
    return true;
  }

  if (IsEquality)
    return false;

  // For two constants C0 and C1 from
  // ```
  // li Y, C0
  // li Z, C1
  // ```
  // 1. if C1 = C0 + 1
  // we can turn:
  //  (a) blt Y, X -> bge X, Z
  //  (b) bge Y, X -> blt X, Z
  //
  // 2. if C1 = C0 - 1
  // we can turn:
  //  (a) blt X, Y -> bge Z, X
  //  (b) bge X, Y -> blt Z, X
  //
  // To make sure this optimization is really beneficial, we only
  // optimize for cases where Y had only one use (i.e. only used by the branch).
  // Try to find the register for constant Z; return
  // invalid register otherwise.
  auto searchConst = [&](int64_t C1) -> Register {
    MachineBasicBlock::reverse_iterator II(&MI), E = MBB->rend();
    auto DefC1 = std::find_if(++II, E, [&](const MachineInstr &I) -> bool {
      int64_t Imm;
      return isLoadImm(&I, Imm) && Imm == C1 &&
             I.getOperand(0).getReg().isVirtual();
    });
    if (DefC1 != E)
      return DefC1->getOperand(0).getReg();

    return Register();
  };

  unsigned NewOpc = RVTrimCC::getBrCond(getInverseBranchCondition(CC));

  // Might be case 1.
  // Don't change 0 to 1 since we can use x0.
  // For unsigned cases changing -1U to 0 would be incorrect.
  // The incorrect case for signed would be INT_MAX, but isFromLoadImm can't
  // return that.
  if (isFromLoadImm(MRI, LHS, C0) && C0 != 0 && LHS.getReg().isVirtual() &&
      MRI.hasOneUse(LHS.getReg()) && (IsSigned || C0 != -1)) {
    assert((isInt<12>(C0) || C0 == 2048) && "Unexpected immediate");
    if (Register RegZ = searchConst(C0 + 1)) {
      BuildMI(*MBB, MI, MI.getDebugLoc(), get(NewOpc))
          .add(RHS)
          .addReg(RegZ)
          .addMBB(TBB);
      // We might extend the live range of Z, clear its kill flag to
      // account for this.
      MRI.clearKillFlags(RegZ);
      MI.eraseFromParent();
      return true;
    }
  }

  // Might be case 2.
  // For signed cases we don't want to change 0 since we can use x0.
  // For unsigned cases changing 0 to -1U would be incorrect.
  // The incorrect case for signed would be INT_MIN, but isFromLoadImm can't
  // return that.
  if (isFromLoadImm(MRI, RHS, C0) && C0 != 0 && RHS.getReg().isVirtual() &&
      MRI.hasOneUse(RHS.getReg())) {
    assert((isInt<12>(C0) || C0 == 2048) && "Unexpected immediate");
    if (Register RegZ = searchConst(C0 - 1)) {
      BuildMI(*MBB, MI, MI.getDebugLoc(), get(NewOpc))
          .addReg(RegZ)
          .add(LHS)
          .addMBB(TBB);
      // We might extend the live range of Z, clear its kill flag to
      // account for this.
      MRI.clearKillFlags(RegZ);
      MI.eraseFromParent();
      return true;
    }
  }

  return false;
}

MachineBasicBlock *
RVTrimInstrInfo::getBranchDestBlock(const MachineInstr &MI) const {
  assert(MI.getDesc().isBranch() && "Unexpected opcode!");
  // The branch target is always the last operand.
  int NumOp = MI.getNumExplicitOperands();
  return MI.getOperand(NumOp - 1).getMBB();
}

bool RVTrimInstrInfo::isBranchOffsetInRange(unsigned BranchOp,
                                           int64_t BrOffset) const {
  unsigned XLen = STI.getXLen();
  // Ideally we could determine the supported branch offset from the
  // RVTrimII::FormMask, but this can't be used for Pseudo instructions like
  // PseudoBR.
  switch (BranchOp) {
  default:
    llvm_unreachable("Unexpected opcode!");
  case RVTrim::NDS_BBC:
  case RVTrim::NDS_BBS:
  case RVTrim::NDS_BEQC:
  case RVTrim::NDS_BNEC:
    return isInt<11>(BrOffset);
  case RVTrim::BEQ:
  case RVTrim::BNE:
  case RVTrim::BLT:
  case RVTrim::BGE:
  case RVTrim::BLTU:
  case RVTrim::BGEU:
  case RVTrim::BEQI:
  case RVTrim::BNEI:
  case RVTrim::CV_BEQIMM:
  case RVTrim::CV_BNEIMM:
  case RVTrim::QC_BEQI:
  case RVTrim::QC_BNEI:
  case RVTrim::QC_BGEI:
  case RVTrim::QC_BLTI:
  case RVTrim::QC_BLTUI:
  case RVTrim::QC_BGEUI:
  case RVTrim::QC_E_BEQI:
  case RVTrim::QC_E_BNEI:
  case RVTrim::QC_E_BGEI:
  case RVTrim::QC_E_BLTI:
  case RVTrim::QC_E_BLTUI:
  case RVTrim::QC_E_BGEUI:
    return isInt<13>(BrOffset);
  case RVTrim::JAL:
  case RVTrim::PseudoBR:
    return isInt<21>(BrOffset);
  case RVTrim::PseudoJump:
    return isInt<32>(SignExtend64(BrOffset + 0x800, XLen));
  }
}

static bool isJumpTableLoad(const MachineInstr &MI) {
  return any_of(MI.memoperands(), [](const MachineMemOperand *MMO) {
    const PseudoSourceValue *PSV = MMO->getPseudoValue();
    return PSV && PSV->isJumpTable();
  });
}

// We want this instruction to be loading the base address of a jump table into
// a register. This can be PseudoMovAddr/PseudoLLA/LUI(+ADDI)/QC_E_LI.
static int getJumpTableIndexFromBase(const MachineRegisterInfo &MRI,
                                     Register Reg) {
  if (!Reg.isVirtual())
    return -1;
  const MachineInstr *MI = MRI.getUniqueVRegDef(Reg);
  if (!MI)
    return -1;

  for (const MachineOperand &MO : MI->operands())
    if (MO.isJTI())
      return MO.getIndex();

  return -1;
}

// This instruction is used as the base address of a jump table load. We expect
// it to be adding the jump table base address to an index that may be scaled.
static int getJumpTableIndexFromLoadAddr(const MachineRegisterInfo &MRI,
                                         Register Reg) {
  if (!Reg.isVirtual())
    return -1;
  const MachineInstr *MI = MRI.getUniqueVRegDef(Reg);
  if (!MI)
    return -1;

  int JTI;
  switch (MI->getOpcode()) {
  case RVTrim::SH1ADD:
  case RVTrim::SH2ADD:
  case RVTrim::SH3ADD:
    // Only the index should be scaled so we just check the unscaled operand for
    // the base address.
    // TODO: Can the address be SHXADD_UW?
    JTI = getJumpTableIndexFromBase(MRI, MI->getOperand(2).getReg());
    if (JTI >= 0)
      return JTI;
    break;
  case RVTrim::ADD:
    JTI = getJumpTableIndexFromBase(MRI, MI->getOperand(1).getReg());
    if (JTI >= 0)
      return JTI;
    JTI = getJumpTableIndexFromBase(MRI, MI->getOperand(2).getReg());
    if (JTI >= 0)
      return JTI;
    break;
  }

  return -1;
}

// Recursively search for %jump-table.N starting from PseudoBRIND,
// and return the index of %jump-table.N.
//
// One common jump table:
//
//   %base   = PseudoMovAddr/PseudoLLA/LUI(+ADDI)/QC_E_LI %jump-table.N
//   %addr   = SH2ADD %index, %base
//   %entry  = LW %addr, 0 :: (load from jump-table)
//   %target = ADD %entry, %base
//   PseudoBRIND %target, 0
//
int RVTrimInstrInfo::getJumpTableIndex(const MachineInstr &MI) const {
  if (MI.getOpcode() != RVTrim::PseudoBRIND &&
      MI.getOpcode() != RVTrim::PseudoBRINDX7)
    return -1;

  Register Reg = MI.getOperand(0).getReg();
  if (!Reg.isVirtual())
    return -1;

  const MachineRegisterInfo &MRI = MI.getMF()->getRegInfo();
  MachineInstr *Def = MRI.getUniqueVRegDef(Reg);
  if (!Def)
    return -1;

  // The target may come directly from a load or the jump table may store
  // relative offset that needs the table base added to it.
  int JTI;
  switch (Def->getOpcode()) {
  case RVTrim::LW:
  case RVTrim::LWU:
  case RVTrim::LD:
    if (!isJumpTableLoad(*Def))
      return -1;

    JTI = getJumpTableIndexFromLoadAddr(MRI, Def->getOperand(1).getReg());
    if (JTI >= 0)
      return JTI;
    break;
  case RVTrim::LXSW:
  case RVTrim::LXWU:
  case RVTrim::LXSD:
  case RVTrim::QC_LRW:
    if (!isJumpTableLoad(*Def))
      return -1;

    JTI = getJumpTableIndexFromBase(MRI, Def->getOperand(1).getReg());
    if (JTI >= 0)
      return JTI;
    break;
  case RVTrim::ADD:
    JTI = getJumpTableIndexFromBase(MRI, Def->getOperand(1).getReg());
    if (JTI >= 0)
      return JTI;
    JTI = getJumpTableIndexFromBase(MRI, Def->getOperand(2).getReg());
    if (JTI >= 0)
      return JTI;
    break;
  }

  return -1;
}

// If the operation has a predicated pseudo instruction, return the pseudo
// instruction opcode. Otherwise, return RVTrim::INSTRUCTION_LIST_END.
// TODO: Support more operations.
unsigned getPredicatedOpcode(unsigned Opcode) {
  // clang-format off
  switch (Opcode) {
  case RVTrim::ADD:   return RVTrim::PseudoCCADD;
  case RVTrim::SUB:   return RVTrim::PseudoCCSUB;
  case RVTrim::SLL:   return RVTrim::PseudoCCSLL;
  case RVTrim::SRL:   return RVTrim::PseudoCCSRL;
  case RVTrim::SRA:   return RVTrim::PseudoCCSRA;
  case RVTrim::AND:   return RVTrim::PseudoCCAND;
  case RVTrim::OR:    return RVTrim::PseudoCCOR;
  case RVTrim::XOR:   return RVTrim::PseudoCCXOR;
  case RVTrim::MAX:   return RVTrim::PseudoCCMAX;
  case RVTrim::MAXU:  return RVTrim::PseudoCCMAXU;
  case RVTrim::MIN:   return RVTrim::PseudoCCMIN;
  case RVTrim::MINU:  return RVTrim::PseudoCCMINU;
  case RVTrim::MUL:   return RVTrim::PseudoCCMUL;
  case RVTrim::LUI:   return RVTrim::PseudoCCLUI;
  case RVTrim::QC_LI:   return RVTrim::PseudoCCQC_LI;
  case RVTrim::QC_E_LI:   return RVTrim::PseudoCCQC_E_LI;

  case RVTrim::ADDI:  return RVTrim::PseudoCCADDI;
  case RVTrim::SLLI:  return RVTrim::PseudoCCSLLI;
  case RVTrim::SRLI:  return RVTrim::PseudoCCSRLI;
  case RVTrim::SRAI:  return RVTrim::PseudoCCSRAI;
  case RVTrim::ANDI:  return RVTrim::PseudoCCANDI;
  case RVTrim::ORI:   return RVTrim::PseudoCCORI;
  case RVTrim::XORI:  return RVTrim::PseudoCCXORI;

  case RVTrim::ADDW:  return RVTrim::PseudoCCADDW;
  case RVTrim::SUBW:  return RVTrim::PseudoCCSUBW;
  case RVTrim::SLLW:  return RVTrim::PseudoCCSLLW;
  case RVTrim::SRLW:  return RVTrim::PseudoCCSRLW;
  case RVTrim::SRAW:  return RVTrim::PseudoCCSRAW;

  case RVTrim::ADDIW: return RVTrim::PseudoCCADDIW;
  case RVTrim::SLLIW: return RVTrim::PseudoCCSLLIW;
  case RVTrim::SRLIW: return RVTrim::PseudoCCSRLIW;
  case RVTrim::SRAIW: return RVTrim::PseudoCCSRAIW;

  case RVTrim::ANDN:  return RVTrim::PseudoCCANDN;
  case RVTrim::ORN:   return RVTrim::PseudoCCORN;
  case RVTrim::XNOR:  return RVTrim::PseudoCCXNOR;

  case RVTrim::NDS_BFOS:  return RVTrim::PseudoCCNDS_BFOS;
  case RVTrim::NDS_BFOZ:  return RVTrim::PseudoCCNDS_BFOZ;
  }
  // clang-format on

  return RVTrim::INSTRUCTION_LIST_END;
}

/// Identify instructions that can be folded into a CCMOV instruction, and
/// return the defining instruction.
static MachineInstr *canFoldAsPredicatedOp(Register Reg,
                                           const MachineRegisterInfo &MRI,
                                           const TargetInstrInfo *TII,
                                           const RVTrimSubtarget &STI) {
  if (!Reg.isVirtual())
    return nullptr;
  if (!MRI.hasOneNonDBGUse(Reg))
    return nullptr;
  MachineInstr *MI = MRI.getVRegDef(Reg);
  if (!MI)
    return nullptr;

  if (!STI.hasShortForwardBranchIMinMax() &&
      (MI->getOpcode() == RVTrim::MAX || MI->getOpcode() == RVTrim::MIN ||
       MI->getOpcode() == RVTrim::MINU || MI->getOpcode() == RVTrim::MAXU))
    return nullptr;

  if (!STI.hasShortForwardBranchIMul() && MI->getOpcode() == RVTrim::MUL)
    return nullptr;

  // Check if MI can be predicated and folded into the CCMOV.
  if (getPredicatedOpcode(MI->getOpcode()) == RVTrim::INSTRUCTION_LIST_END)
    return nullptr;
  // Don't predicate li idiom.
  if (MI->getOpcode() == RVTrim::ADDI && MI->getOperand(1).isReg() &&
      MI->getOperand(1).getReg() == RVTrim::X0)
    return nullptr;
  // Check if MI has any other defs or physreg uses.
  for (const MachineOperand &MO : llvm::drop_begin(MI->operands())) {
    // Reject frame index operands, PEI can't handle the predicated pseudos.
    if (MO.isFI() || MO.isCPI() || MO.isJTI())
      return nullptr;
    if (!MO.isReg())
      continue;
    // MI can't have any tied operands, that would conflict with predication.
    if (MO.isTied())
      return nullptr;
    if (MO.isDef())
      return nullptr;
    // Allow constant physregs.
    if (MO.getReg().isPhysical() && !MRI.isConstantPhysReg(MO.getReg()))
      return nullptr;
  }
  bool DontMoveAcrossStores = true;
  if (!MI->isSafeToMove(DontMoveAcrossStores))
    return nullptr;
  return MI;
}

MachineInstr *
RVTrimInstrInfo::optimizeSelect(MachineInstr &MI,
                               SmallPtrSetImpl<MachineInstr *> &SeenMIs,
                               bool PreferFalse) const {
  assert(MI.getOpcode() == RVTrim::PseudoCCMOVGPR &&
         "Unknown select instruction");
  if (!STI.hasShortForwardBranchIALU())
    return nullptr;

  MachineRegisterInfo &MRI = MI.getParent()->getParent()->getRegInfo();
  MachineInstr *DefMI =
      canFoldAsPredicatedOp(MI.getOperand(2).getReg(), MRI, this, STI);
  bool Invert = !DefMI;
  if (!DefMI)
    DefMI = canFoldAsPredicatedOp(MI.getOperand(1).getReg(), MRI, this, STI);
  if (!DefMI)
    return nullptr;

  // Find new register class to use.
  MachineOperand FalseReg = MI.getOperand(Invert ? 2 : 1);
  Register DestReg = MI.getOperand(0).getReg();
  const TargetRegisterClass *PreviousClass = MRI.getRegClass(FalseReg.getReg());
  if (!MRI.constrainRegClass(DestReg, PreviousClass))
    return nullptr;

  unsigned PredOpc = getPredicatedOpcode(DefMI->getOpcode());
  assert(PredOpc != RVTrim::INSTRUCTION_LIST_END && "Unexpected opcode!");

  // Create a new predicated version of DefMI.
  MachineInstrBuilder NewMI =
      BuildMI(*MI.getParent(), MI, MI.getDebugLoc(), get(PredOpc), DestReg);

  // Copy the false register.
  NewMI.add(FalseReg);

  // Copy all the DefMI operands.
  const MCInstrDesc &DefDesc = DefMI->getDesc();
  for (unsigned i = 1, e = DefDesc.getNumOperands(); i != e; ++i)
    NewMI.add(DefMI->getOperand(i));

  // Add branch opcode, inverting if necessary.
  unsigned BCCOpcode = MI.getOperand(MI.getNumExplicitOperands() - 3).getImm();
  if (Invert)
    BCCOpcode = RVTrimCC::getInverseBranchOpcode(BCCOpcode);
  NewMI.addImm(BCCOpcode);

  // Copy the condition portion.
  NewMI.add(MI.getOperand(MI.getNumExplicitOperands() - 2));
  NewMI.add(MI.getOperand(MI.getNumExplicitOperands() - 1));

  // Update SeenMIs set: register newly created MI and erase removed DefMI.
  SeenMIs.insert(NewMI);
  SeenMIs.erase(DefMI);

  // If MI is inside a loop, and DefMI is outside the loop, then kill flags on
  // DefMI would be invalid when transferred inside the loop.  Checking for a
  // loop is expensive, but at least remove kill flags if they are in different
  // BBs.
  if (DefMI->getParent() != MI.getParent())
    NewMI->clearKillInfo();

  // The caller will erase MI, but not DefMI.
  DefMI->eraseFromParent();
  return NewMI;
}

unsigned RVTrimInstrInfo::getInstSizeInBytes(const MachineInstr &MI) const {
  if (MI.isMetaInstruction())
    return 0;

  unsigned Opcode = MI.getOpcode();

  if (Opcode == TargetOpcode::INLINEASM ||
      Opcode == TargetOpcode::INLINEASM_BR) {
    const MachineFunction &MF = *MI.getParent()->getParent();
    return getInlineAsmLength(MI.getOperand(0).getSymbolName(),
                              MF.getTarget().getMCAsmInfo());
  }

  if (requiresNTLHint(MI)) {
    if (STI.hasStdExtZca()) {
      if (unsigned Size = getCompressedSize(MI, STI))
        return 2 + Size; // c.ntl.all + c.load/c.store
      return 6;   // c.ntl.all + load/store
    }
    return 8; // ntl.all + load/store
  }

  if (Opcode == TargetOpcode::BUNDLE)
    return getInstBundleSize(MI);

  if (MI.getParent() && MI.getParent()->getParent()) {
    if (unsigned Size = getCompressedSize(MI, STI))
      return Size;
  }

  switch (Opcode) {
  case RVTrim::PseudoMV_FPR16INX:
  case RVTrim::PseudoMV_FPR32INX:
  case RVTrim::PseudoClearGPR:
    // MV is always compressible to either c.mv or c.li rd, 0.
    return STI.hasStdExtZca() ? 2 : 4;
  // Below cases are for short forward branch pseudos
  case RVTrim::PseudoCCMOVGPRNoX0:
    return get(MI.getOperand(MI.getNumExplicitOperands() - 3).getImm())
               .getSize() +
           2;
  case RVTrim::PseudoCCMOVGPR:
  case RVTrim::PseudoCCADD:
  case RVTrim::PseudoCCSUB:
  case RVTrim::PseudoCCSLL:
  case RVTrim::PseudoCCSRL:
  case RVTrim::PseudoCCSRA:
  case RVTrim::PseudoCCAND:
  case RVTrim::PseudoCCOR:
  case RVTrim::PseudoCCXOR:
  case RVTrim::PseudoCCADDI:
  case RVTrim::PseudoCCANDI:
  case RVTrim::PseudoCCORI:
  case RVTrim::PseudoCCXORI:
  case RVTrim::PseudoCCLUI:
  case RVTrim::PseudoCCSLLI:
  case RVTrim::PseudoCCSRLI:
  case RVTrim::PseudoCCSRAI:
  case RVTrim::PseudoCCADDW:
  case RVTrim::PseudoCCSUBW:
  case RVTrim::PseudoCCSLLW:
  case RVTrim::PseudoCCSRLW:
  case RVTrim::PseudoCCSRAW:
  case RVTrim::PseudoCCADDIW:
  case RVTrim::PseudoCCSLLIW:
  case RVTrim::PseudoCCSRLIW:
  case RVTrim::PseudoCCSRAIW:
  case RVTrim::PseudoCCANDN:
  case RVTrim::PseudoCCORN:
  case RVTrim::PseudoCCXNOR:
  case RVTrim::PseudoCCMAX:
  case RVTrim::PseudoCCMIN:
  case RVTrim::PseudoCCMAXU:
  case RVTrim::PseudoCCMINU:
  case RVTrim::PseudoCCMUL:
  case RVTrim::PseudoCCLB:
  case RVTrim::PseudoCCLH:
  case RVTrim::PseudoCCLW:
  case RVTrim::PseudoCCLHU:
  case RVTrim::PseudoCCLBU:
  case RVTrim::PseudoCCLWU:
  case RVTrim::PseudoCCLD:
  case RVTrim::PseudoCCQC_LI:
    return get(MI.getOperand(MI.getNumExplicitOperands() - 3).getImm())
               .getSize() +
           4;
  case RVTrim::PseudoCCQC_E_LI:
  case RVTrim::PseudoCCQC_E_LB:
  case RVTrim::PseudoCCQC_E_LH:
  case RVTrim::PseudoCCQC_E_LW:
  case RVTrim::PseudoCCQC_E_LHU:
  case RVTrim::PseudoCCQC_E_LBU:
    return get(MI.getOperand(MI.getNumExplicitOperands() - 3).getImm())
               .getSize() +
           6;
  case TargetOpcode::STACKMAP:
    // The upper bound for a stackmap intrinsic is the full length of its shadow
    return StackMapOpers(&MI).getNumPatchBytes();
  case TargetOpcode::PATCHPOINT:
    // The size of the patchpoint intrinsic is the number of bytes requested
    return PatchPointOpers(&MI).getNumPatchBytes();
  case TargetOpcode::STATEPOINT: {
    // The size of the statepoint intrinsic is the number of bytes requested
    unsigned NumBytes = StatepointOpers(&MI).getNumPatchBytes();
    // No patch bytes means at most a PseudoCall is emitted
    return std::max(NumBytes, 8U);
  }
  case TargetOpcode::PATCHABLE_FUNCTION_ENTER:
  case TargetOpcode::PATCHABLE_FUNCTION_EXIT:
  case TargetOpcode::PATCHABLE_TAIL_CALL: {
    const MachineFunction &MF = *MI.getParent()->getParent();
    const Function &F = MF.getFunction();
    if (Opcode == TargetOpcode::PATCHABLE_FUNCTION_ENTER &&
        F.hasFnAttribute("patchable-function-entry")) {
      unsigned Num =
          F.getFnAttributeAsParsedInteger("patchable-function-entry");
      // Number of C.NOP or NOP
      return (STI.hasStdExtZca() ? 2 : 4) * Num;
    }
    // XRay uses C.JAL + 21 or 33 C.NOP for each sled in RV32 and RV64,
    // respectively.
    return STI.is64Bit() ? 68 : 44;
  }
  default:
    return get(Opcode).getSize();
  }
}

bool RVTrimInstrInfo::isAsCheapAsAMove(const MachineInstr &MI) const {
  const unsigned Opcode = MI.getOpcode();
  switch (Opcode) {
  default:
    break;
  case RVTrim::FSGNJ_D:
  case RVTrim::FSGNJ_S:
  case RVTrim::FSGNJ_H:
  case RVTrim::FSGNJ_D_INX:
  case RVTrim::FSGNJ_D_IN32X:
  case RVTrim::FSGNJ_S_INX:
  case RVTrim::FSGNJ_H_INX:
    // The canonical floating-point move is fsgnj rd, rs, rs.
    return MI.getOperand(1).isReg() && MI.getOperand(2).isReg() &&
           MI.getOperand(1).getReg() == MI.getOperand(2).getReg();
  case RVTrim::ADDI:
  case RVTrim::ORI:
  case RVTrim::XORI:
    return (MI.getOperand(1).isReg() &&
            MI.getOperand(1).getReg() == RVTrim::X0) ||
           (MI.getOperand(2).isImm() && MI.getOperand(2).getImm() == 0);
  }
  return MI.isAsCheapAsAMove();
}

std::optional<DestSourcePair>
RVTrimInstrInfo::isCopyInstrImpl(const MachineInstr &MI) const {
  if (MI.isMoveReg())
    return DestSourcePair{MI.getOperand(0), MI.getOperand(1)};
  switch (MI.getOpcode()) {
  default:
    break;
  case RVTrim::ADD:
  case RVTrim::OR:
  case RVTrim::XOR:
    if (MI.getOperand(1).isReg() && MI.getOperand(1).getReg() == RVTrim::X0 &&
        MI.getOperand(2).isReg())
      return DestSourcePair{MI.getOperand(0), MI.getOperand(2)};
    if (MI.getOperand(2).isReg() && MI.getOperand(2).getReg() == RVTrim::X0 &&
        MI.getOperand(1).isReg())
      return DestSourcePair{MI.getOperand(0), MI.getOperand(1)};
    break;
  case RVTrim::ADDI:
    // Operand 1 can be a frameindex but callers expect registers
    if (MI.getOperand(1).isReg() && MI.getOperand(2).isImm() &&
        MI.getOperand(2).getImm() == 0)
      return DestSourcePair{MI.getOperand(0), MI.getOperand(1)};
    break;
  case RVTrim::SUB:
    if (MI.getOperand(2).isReg() && MI.getOperand(2).getReg() == RVTrim::X0 &&
        MI.getOperand(1).isReg())
      return DestSourcePair{MI.getOperand(0), MI.getOperand(1)};
    break;
  case RVTrim::SH1ADD:
  case RVTrim::SH1ADD_UW:
  case RVTrim::SH2ADD:
  case RVTrim::SH2ADD_UW:
  case RVTrim::SH3ADD:
  case RVTrim::SH3ADD_UW:
    if (MI.getOperand(1).isReg() && MI.getOperand(1).getReg() == RVTrim::X0 &&
        MI.getOperand(2).isReg())
      return DestSourcePair{MI.getOperand(0), MI.getOperand(2)};
    break;
  case RVTrim::FSGNJ_D:
  case RVTrim::FSGNJ_S:
  case RVTrim::FSGNJ_H:
  case RVTrim::FSGNJ_D_INX:
  case RVTrim::FSGNJ_D_IN32X:
  case RVTrim::FSGNJ_S_INX:
  case RVTrim::FSGNJ_H_INX:
    // The canonical floating-point move is fsgnj rd, rs, rs.
    if (MI.getOperand(1).isReg() && MI.getOperand(2).isReg() &&
        MI.getOperand(1).getReg() == MI.getOperand(2).getReg())
      return DestSourcePair{MI.getOperand(0), MI.getOperand(1)};
    break;
  }
  return std::nullopt;
}

MachineTraceStrategy RVTrimInstrInfo::getMachineCombinerTraceStrategy() const {
  std::optional<MachineTraceStrategy> Forced =
      STI.getCLOpts().force_machine_combiner_strategy;
  if (!Forced) {
    // The option is unused. Choose Local strategy only for in-order cores. When
    // scheduling model is unspecified, use MinInstrCount strategy as more
    // generic one.
    const auto &SchedModel = STI.getSchedModel();
    return (!SchedModel.hasInstrSchedModel() || SchedModel.isOutOfOrder())
               ? MachineTraceStrategy::TS_MinInstrCount
               : MachineTraceStrategy::TS_Local;
  }
  // The strategy was forced by the option.
  return *Forced;
}

void RVTrimInstrInfo::finalizeInsInstrs(
    MachineInstr &Root, unsigned &Pattern,
    SmallVectorImpl<MachineInstr *> &InsInstrs) const {
  int16_t FrmOpIdx =
      RVTrim::getNamedOperandIdx(Root.getOpcode(), RVTrim::OpName::frm);
  if (FrmOpIdx < 0) {
    assert(all_of(InsInstrs,
                  [](MachineInstr *MI) {
                    return RVTrim::getNamedOperandIdx(MI->getOpcode(),
                                                     RVTrim::OpName::frm) < 0;
                  }) &&
           "New instructions require FRM whereas the old one does not have it");
    return;
  }

  const MachineOperand &FRM = Root.getOperand(FrmOpIdx);
  MachineFunction &MF = *Root.getMF();

  for (auto *NewMI : InsInstrs) {
    // We'd already added the FRM operand.
    if (static_cast<unsigned>(RVTrim::getNamedOperandIdx(
            NewMI->getOpcode(), RVTrim::OpName::frm)) != NewMI->getNumOperands())
      continue;
    MachineInstrBuilder MIB(MF, NewMI);
    MIB.add(FRM);
    if (FRM.getImm() == RVTrimFPRndMode::DYN)
      MIB.addUse(RVTrim::FRM, RegState::Implicit);
  }
}

static bool isFADD(unsigned Opc) {
  switch (Opc) {
  default:
    return false;
  case RVTrim::FADD_H:
  case RVTrim::FADD_S:
  case RVTrim::FADD_D:
    return true;
  }
}

static bool isFSUB(unsigned Opc) {
  switch (Opc) {
  default:
    return false;
  case RVTrim::FSUB_H:
  case RVTrim::FSUB_S:
  case RVTrim::FSUB_D:
    return true;
  }
}

static bool isFMUL(unsigned Opc) {
  switch (Opc) {
  default:
    return false;
  case RVTrim::FMUL_H:
  case RVTrim::FMUL_S:
  case RVTrim::FMUL_D:
    return true;
  }
}

bool RVTrimInstrInfo::isVectorAssociativeAndCommutative(const MachineInstr &Inst,
                                                       bool Invert) const {
#define OPCODE_LMUL_CASE(OPC)                                                  \
  case RVTrim::OPC##_M1:                                                        \
  case RVTrim::OPC##_M2:                                                        \
  case RVTrim::OPC##_M4:                                                        \
  case RVTrim::OPC##_M8:                                                        \
  case RVTrim::OPC##_MF2:                                                       \
  case RVTrim::OPC##_MF4:                                                       \
  case RVTrim::OPC##_MF8

#define OPCODE_LMUL_MASK_CASE(OPC)                                             \
  case RVTrim::OPC##_M1_MASK:                                                   \
  case RVTrim::OPC##_M2_MASK:                                                   \
  case RVTrim::OPC##_M4_MASK:                                                   \
  case RVTrim::OPC##_M8_MASK:                                                   \
  case RVTrim::OPC##_MF2_MASK:                                                  \
  case RVTrim::OPC##_MF4_MASK:                                                  \
  case RVTrim::OPC##_MF8_MASK

  unsigned Opcode = Inst.getOpcode();
  if (Invert) {
    if (auto InvOpcode = getInverseOpcode(Opcode))
      Opcode = *InvOpcode;
    else
      return false;
  }

  // clang-format off
  switch (Opcode) {
  default:
    return false;
  OPCODE_LMUL_CASE(PseudoVADD_VV):
  OPCODE_LMUL_MASK_CASE(PseudoVADD_VV):
  OPCODE_LMUL_CASE(PseudoVMUL_VV):
  OPCODE_LMUL_MASK_CASE(PseudoVMUL_VV):
    return true;
  }
  // clang-format on

#undef OPCODE_LMUL_MASK_CASE
#undef OPCODE_LMUL_CASE
}

bool RVTrimInstrInfo::areRVVInstsReassociable(const MachineInstr &Root,
                                             const MachineInstr &Prev) const {
  if (!areOpcodesEqualOrInverse(Root.getOpcode(), Prev.getOpcode()))
    return false;

  assert(Root.getMF() == Prev.getMF());
  const MachineRegisterInfo *MRI = &Root.getMF()->getRegInfo();
  const TargetRegisterInfo *TRI = MRI->getTargetRegisterInfo();

  // Make sure vtype operands are also the same.
  const MCInstrDesc &Desc = get(Root.getOpcode());
  const uint64_t TSFlags = Desc.TSFlags;

  auto checkImmOperand = [&](unsigned OpIdx) {
    return Root.getOperand(OpIdx).getImm() == Prev.getOperand(OpIdx).getImm();
  };

  auto checkRegOperand = [&](unsigned OpIdx) {
    return Root.getOperand(OpIdx).getReg() == Prev.getOperand(OpIdx).getReg();
  };

  // PassThru
  // TODO: Potentially we can loosen the condition to consider Root to be
  // associable with Prev if Root has NoReg as passthru. In which case we
  // also need to loosen the condition on vector policies between these.
  if (!checkRegOperand(1))
    return false;

  // SEW
  if (RVTrimII::hasSEWOp(TSFlags) &&
      !checkImmOperand(RVTrimII::getSEWOpNum(Desc)))
    return false;

  // Mask
  if (RVTrimII::usesMaskPolicy(TSFlags)) {
    const MachineBasicBlock *MBB = Root.getParent();
    const MachineBasicBlock::const_reverse_iterator It1(&Root);
    const MachineBasicBlock::const_reverse_iterator It2(&Prev);
    Register MI1VReg;

    bool SeenMI2 = false;
    for (auto End = MBB->rend(), It = It1; It != End; ++It) {
      if (It == It2) {
        SeenMI2 = true;
        if (!MI1VReg.isValid())
          // There is no V0 def between Root and Prev; they're sharing the
          // same V0.
          break;
      }

      if (It->modifiesRegister(RVTrim::V0, TRI)) {
        Register SrcReg = It->getOperand(1).getReg();
        // If it's not VReg it'll be more difficult to track its defs, so
        // bailing out here just to be safe.
        if (!SrcReg.isVirtual())
          return false;

        if (!MI1VReg.isValid()) {
          // This is the V0 def for Root.
          MI1VReg = SrcReg;
          continue;
        }

        // Some random mask updates.
        if (!SeenMI2)
          continue;

        // This is the V0 def for Prev; check if it's the same as that of
        // Root.
        if (MI1VReg != SrcReg)
          return false;
        else
          break;
      }
    }

    // If we haven't encountered Prev, it's likely that this function was
    // called in a wrong way (e.g. Root is before Prev).
    assert(SeenMI2 && "Prev is expected to appear before Root");
  }

  // Tail / Mask policies
  if (RVTrimII::hasVecPolicyOp(TSFlags) &&
      !checkImmOperand(RVTrimII::getVecPolicyOpNum(Desc)))
    return false;

  // VL
  if (RVTrimII::hasVLOp(TSFlags)) {
    unsigned OpIdx = RVTrimII::getVLOpNum(Desc);
    const MachineOperand &Op1 = Root.getOperand(OpIdx);
    const MachineOperand &Op2 = Prev.getOperand(OpIdx);
    if (Op1.getType() != Op2.getType())
      return false;
    switch (Op1.getType()) {
    case MachineOperand::MO_Register:
      if (Op1.getReg() != Op2.getReg())
        return false;
      break;
    case MachineOperand::MO_Immediate:
      if (Op1.getImm() != Op2.getImm())
        return false;
      break;
    default:
      llvm_unreachable("Unrecognized VL operand type");
    }
  }

  // Rounding modes
  if (int Idx = RVTrimII::getFRMOpNum(Desc); Idx >= 0 && !checkImmOperand(Idx))
    return false;
  if (int Idx = RVTrimII::getVXRMOpNum(Desc); Idx >= 0 && !checkImmOperand(Idx))
    return false;

  return true;
}

// Most of our RVV pseudos have passthru operand, so the real operands
// start from index = 2.
bool RVTrimInstrInfo::hasReassociableVectorSibling(const MachineInstr &Inst,
                                                  bool &Commuted) const {
  const MachineBasicBlock *MBB = Inst.getParent();
  const MachineRegisterInfo &MRI = MBB->getParent()->getRegInfo();
  assert(RVTrimII::isFirstDefTiedToFirstUse(get(Inst.getOpcode())) &&
         "Expect the present of passthrough operand.");
  MachineInstr *MI1 = MRI.getUniqueVRegDef(Inst.getOperand(2).getReg());
  MachineInstr *MI2 = MRI.getUniqueVRegDef(Inst.getOperand(3).getReg());

  // If only one operand has the same or inverse opcode and it's the second
  // source operand, the operands must be commuted.
  Commuted = !areRVVInstsReassociable(Inst, *MI1) &&
             areRVVInstsReassociable(Inst, *MI2);
  if (Commuted)
    std::swap(MI1, MI2);

  return areRVVInstsReassociable(Inst, *MI1) &&
         (isVectorAssociativeAndCommutative(*MI1) ||
          isVectorAssociativeAndCommutative(*MI1, /* Invert */ true)) &&
         hasReassociableOperands(*MI1, MBB) &&
         MRI.hasOneNonDBGUse(MI1->getOperand(0).getReg());
}

bool RVTrimInstrInfo::hasReassociableOperands(
    const MachineInstr &Inst, const MachineBasicBlock *MBB) const {
  if (!isVectorAssociativeAndCommutative(Inst) &&
      !isVectorAssociativeAndCommutative(Inst, /*Invert=*/true))
    return TargetInstrInfo::hasReassociableOperands(Inst, MBB);

  const MachineOperand &Op1 = Inst.getOperand(2);
  const MachineOperand &Op2 = Inst.getOperand(3);
  const MachineRegisterInfo &MRI = MBB->getParent()->getRegInfo();

  // We need virtual register definitions for the operands that we will
  // reassociate.
  MachineInstr *MI1 = nullptr;
  MachineInstr *MI2 = nullptr;
  if (Op1.isReg() && Op1.getReg().isVirtual())
    MI1 = MRI.getUniqueVRegDef(Op1.getReg());
  if (Op2.isReg() && Op2.getReg().isVirtual())
    MI2 = MRI.getUniqueVRegDef(Op2.getReg());

  // And at least one operand must be defined in MBB.
  return MI1 && MI2 && (MI1->getParent() == MBB || MI2->getParent() == MBB);
}

void RVTrimInstrInfo::getReassociateOperandIndices(
    const MachineInstr &Root, unsigned Pattern,
    std::array<unsigned, 5> &OperandIndices) const {
  TargetInstrInfo::getReassociateOperandIndices(Root, Pattern, OperandIndices);
  if (RVTrim::getRVVMCOpcode(Root.getOpcode())) {
    // Skip the passthrough operand, so increment all indices by one.
    for (unsigned I = 0; I < 5; ++I)
      ++OperandIndices[I];
  }
}

bool RVTrimInstrInfo::hasReassociableSibling(const MachineInstr &Inst,
                                            bool &Commuted) const {
  if (isVectorAssociativeAndCommutative(Inst) ||
      isVectorAssociativeAndCommutative(Inst, /*Invert=*/true))
    return hasReassociableVectorSibling(Inst, Commuted);

  if (!TargetInstrInfo::hasReassociableSibling(Inst, Commuted))
    return false;

  const MachineRegisterInfo &MRI = Inst.getMF()->getRegInfo();
  unsigned OperandIdx = Commuted ? 2 : 1;
  const MachineInstr &Sibling =
      *MRI.getVRegDef(Inst.getOperand(OperandIdx).getReg());

  int16_t InstFrmOpIdx =
      RVTrim::getNamedOperandIdx(Inst.getOpcode(), RVTrim::OpName::frm);
  int16_t SiblingFrmOpIdx =
      RVTrim::getNamedOperandIdx(Sibling.getOpcode(), RVTrim::OpName::frm);

  return (InstFrmOpIdx < 0 && SiblingFrmOpIdx < 0) ||
         RVTrim::hasEqualFRM(Inst, Sibling);
}

bool RVTrimInstrInfo::isAssociativeAndCommutative(const MachineInstr &Inst,
                                                 bool Invert) const {
  if (isVectorAssociativeAndCommutative(Inst, Invert))
    return true;

  unsigned Opc = Inst.getOpcode();
  if (Invert) {
    auto InverseOpcode = getInverseOpcode(Opc);
    if (!InverseOpcode)
      return false;
    Opc = *InverseOpcode;
  }

  if (isFADD(Opc) || isFMUL(Opc))
    return Inst.getFlag(MachineInstr::MIFlag::FmReassoc) &&
           Inst.getFlag(MachineInstr::MIFlag::FmNsz);

  switch (Opc) {
  default:
    return false;
  case RVTrim::ADD:
  case RVTrim::ADDW:
  case RVTrim::AND:
  case RVTrim::OR:
  case RVTrim::XOR:
  // From RISC-V ISA spec, if both the high and low bits of the same product
  // are required, then the recommended code sequence is:
  //
  // MULH[[S]U] rdh, rs1, rs2
  // MUL        rdl, rs1, rs2
  // (source register specifiers must be in same order and rdh cannot be the
  //  same as rs1 or rs2)
  //
  // Microarchitectures can then fuse these into a single multiply operation
  // instead of performing two separate multiplies.
  // MachineCombiner may reassociate MUL operands and lose the fusion
  // opportunity.
  case RVTrim::MUL:
  case RVTrim::MULW:
  case RVTrim::MIN:
  case RVTrim::MINU:
  case RVTrim::MAX:
  case RVTrim::MAXU:
  case RVTrim::FMIN_H:
  case RVTrim::FMIN_S:
  case RVTrim::FMIN_D:
  case RVTrim::FMAX_H:
  case RVTrim::FMAX_S:
  case RVTrim::FMAX_D:
    return true;
  }

  return false;
}

std::optional<unsigned>
RVTrimInstrInfo::getInverseOpcode(unsigned Opcode) const {
#define RVV_OPC_LMUL_CASE(OPC, INV)                                            \
  case RVTrim::OPC##_M1:                                                        \
    return RVTrim::INV##_M1;                                                    \
  case RVTrim::OPC##_M2:                                                        \
    return RVTrim::INV##_M2;                                                    \
  case RVTrim::OPC##_M4:                                                        \
    return RVTrim::INV##_M4;                                                    \
  case RVTrim::OPC##_M8:                                                        \
    return RVTrim::INV##_M8;                                                    \
  case RVTrim::OPC##_MF2:                                                       \
    return RVTrim::INV##_MF2;                                                   \
  case RVTrim::OPC##_MF4:                                                       \
    return RVTrim::INV##_MF4;                                                   \
  case RVTrim::OPC##_MF8:                                                       \
    return RVTrim::INV##_MF8

#define RVV_OPC_LMUL_MASK_CASE(OPC, INV)                                       \
  case RVTrim::OPC##_M1_MASK:                                                   \
    return RVTrim::INV##_M1_MASK;                                               \
  case RVTrim::OPC##_M2_MASK:                                                   \
    return RVTrim::INV##_M2_MASK;                                               \
  case RVTrim::OPC##_M4_MASK:                                                   \
    return RVTrim::INV##_M4_MASK;                                               \
  case RVTrim::OPC##_M8_MASK:                                                   \
    return RVTrim::INV##_M8_MASK;                                               \
  case RVTrim::OPC##_MF2_MASK:                                                  \
    return RVTrim::INV##_MF2_MASK;                                              \
  case RVTrim::OPC##_MF4_MASK:                                                  \
    return RVTrim::INV##_MF4_MASK;                                              \
  case RVTrim::OPC##_MF8_MASK:                                                  \
    return RVTrim::INV##_MF8_MASK

  switch (Opcode) {
  default:
    return std::nullopt;
  case RVTrim::FADD_H:
    return RVTrim::FSUB_H;
  case RVTrim::FADD_S:
    return RVTrim::FSUB_S;
  case RVTrim::FADD_D:
    return RVTrim::FSUB_D;
  case RVTrim::FSUB_H:
    return RVTrim::FADD_H;
  case RVTrim::FSUB_S:
    return RVTrim::FADD_S;
  case RVTrim::FSUB_D:
    return RVTrim::FADD_D;
  case RVTrim::ADD:
    return RVTrim::SUB;
  case RVTrim::SUB:
    return RVTrim::ADD;
  case RVTrim::ADDW:
    return RVTrim::SUBW;
  case RVTrim::SUBW:
    return RVTrim::ADDW;
    // clang-format off
  RVV_OPC_LMUL_CASE(PseudoVADD_VV, PseudoVSUB_VV);
  RVV_OPC_LMUL_MASK_CASE(PseudoVADD_VV, PseudoVSUB_VV);
  RVV_OPC_LMUL_CASE(PseudoVSUB_VV, PseudoVADD_VV);
  RVV_OPC_LMUL_MASK_CASE(PseudoVSUB_VV, PseudoVADD_VV);
    // clang-format on
  }

#undef RVV_OPC_LMUL_MASK_CASE
#undef RVV_OPC_LMUL_CASE
}

static bool canCombineFPFusedMultiply(const MachineInstr &Root,
                                      const MachineOperand &MO,
                                      bool DoRegPressureReduce) {
  if (!MO.isReg() || !MO.getReg().isVirtual())
    return false;
  const MachineRegisterInfo &MRI = Root.getMF()->getRegInfo();
  MachineInstr *MI = MRI.getVRegDef(MO.getReg());
  if (!MI || !isFMUL(MI->getOpcode()))
    return false;

  if (!Root.getFlag(MachineInstr::MIFlag::FmContract) ||
      !MI->getFlag(MachineInstr::MIFlag::FmContract))
    return false;

  // Try combining even if fmul has more than one use as it eliminates
  // dependency between fadd(fsub) and fmul. However, it can extend liveranges
  // for fmul operands, so reject the transformation in register pressure
  // reduction mode.
  if (DoRegPressureReduce && !MRI.hasOneNonDBGUse(MI->getOperand(0).getReg()))
    return false;

  // Do not combine instructions from different basic blocks.
  if (Root.getParent() != MI->getParent())
    return false;
  return RVTrim::hasEqualFRM(Root, *MI);
}

static bool getFPFusedMultiplyPatterns(MachineInstr &Root,
                                       SmallVectorImpl<unsigned> &Patterns,
                                       bool DoRegPressureReduce) {
  unsigned Opc = Root.getOpcode();
  bool IsFAdd = isFADD(Opc);
  if (!IsFAdd && !isFSUB(Opc))
    return false;
  bool Added = false;
  if (canCombineFPFusedMultiply(Root, Root.getOperand(1),
                                DoRegPressureReduce)) {
    Patterns.push_back(IsFAdd ? RVTrimMachineCombinerPattern::FMADD_AX
                              : RVTrimMachineCombinerPattern::FMSUB);
    Added = true;
  }
  if (canCombineFPFusedMultiply(Root, Root.getOperand(2),
                                DoRegPressureReduce)) {
    Patterns.push_back(IsFAdd ? RVTrimMachineCombinerPattern::FMADD_XA
                              : RVTrimMachineCombinerPattern::FNMSUB);
    Added = true;
  }
  return Added;
}

static bool getFPPatterns(MachineInstr &Root,
                          SmallVectorImpl<unsigned> &Patterns,
                          bool DoRegPressureReduce) {
  return getFPFusedMultiplyPatterns(Root, Patterns, DoRegPressureReduce);
}

/// Utility routine that checks if \param MO is defined by an
/// \param CombineOpc instruction in the basic block \param MBB
static const MachineInstr *canCombine(const MachineBasicBlock &MBB,
                                      const MachineOperand &MO,
                                      unsigned CombineOpc) {
  const MachineRegisterInfo &MRI = MBB.getParent()->getRegInfo();
  const MachineInstr *MI = nullptr;

  if (MO.isReg() && MO.getReg().isVirtual())
    MI = MRI.getUniqueVRegDef(MO.getReg());
  // And it needs to be in the trace (otherwise, it won't have a depth).
  if (!MI || MI->getParent() != &MBB || MI->getOpcode() != CombineOpc)
    return nullptr;
  // Must only used by the user we combine with.
  if (!MRI.hasOneNonDBGUse(MI->getOperand(0).getReg()))
    return nullptr;

  return MI;
}

/// Utility routine that checks if \param MO is defined by a SLLI in \param
/// MBB that can be combined by splitting across 2 SHXADD instructions. The
/// first SHXADD shift amount is given by \param OuterShiftAmt.
static bool canCombineShiftIntoShXAdd(const MachineBasicBlock &MBB,
                                      const MachineOperand &MO,
                                      unsigned OuterShiftAmt) {
  const MachineInstr *ShiftMI = canCombine(MBB, MO, RVTrim::SLLI);
  if (!ShiftMI)
    return false;

  unsigned InnerShiftAmt = ShiftMI->getOperand(2).getImm();
  if (InnerShiftAmt < OuterShiftAmt || (InnerShiftAmt - OuterShiftAmt) > 3)
    return false;

  return true;
}

// Returns the shift amount from a SHXADD instruction. Returns 0 if the
// instruction is not a SHXADD.
static unsigned getSHXADDShiftAmount(unsigned Opc) {
  switch (Opc) {
  default:
    return 0;
  case RVTrim::SH1ADD:
    return 1;
  case RVTrim::SH2ADD:
    return 2;
  case RVTrim::SH3ADD:
    return 3;
  }
}

// Returns the shift amount from a SHXADD.UW instruction. Returns 0 if the
// instruction is not a SHXADD.UW.
static unsigned getSHXADDUWShiftAmount(unsigned Opc) {
  switch (Opc) {
  default:
    return 0;
  case RVTrim::SH1ADD_UW:
    return 1;
  case RVTrim::SH2ADD_UW:
    return 2;
  case RVTrim::SH3ADD_UW:
    return 3;
  }
}

// Look for opportunities to combine (sh3add Z, (add X, (slli Y, 5))) into
// (sh3add (sh2add Y, Z), X).
static bool getSHXADDPatterns(const MachineInstr &Root,
                              SmallVectorImpl<unsigned> &Patterns) {
  unsigned ShiftAmt = getSHXADDShiftAmount(Root.getOpcode());
  if (!ShiftAmt)
    return false;

  const MachineBasicBlock &MBB = *Root.getParent();

  const MachineInstr *AddMI = canCombine(MBB, Root.getOperand(2), RVTrim::ADD);
  if (!AddMI)
    return false;

  bool Found = false;
  if (canCombineShiftIntoShXAdd(MBB, AddMI->getOperand(1), ShiftAmt)) {
    Patterns.push_back(RVTrimMachineCombinerPattern::SHXADD_ADD_SLLI_OP1);
    Found = true;
  }
  if (canCombineShiftIntoShXAdd(MBB, AddMI->getOperand(2), ShiftAmt)) {
    Patterns.push_back(RVTrimMachineCombinerPattern::SHXADD_ADD_SLLI_OP2);
    Found = true;
  }

  return Found;
}

CombinerObjective RVTrimInstrInfo::getCombinerObjective(unsigned Pattern) const {
  switch (Pattern) {
  case RVTrimMachineCombinerPattern::FMADD_AX:
  case RVTrimMachineCombinerPattern::FMADD_XA:
  case RVTrimMachineCombinerPattern::FMSUB:
  case RVTrimMachineCombinerPattern::FNMSUB:
    return CombinerObjective::MustReduceDepth;
  default:
    return TargetInstrInfo::getCombinerObjective(Pattern);
  }
}

bool RVTrimInstrInfo::getMachineCombinerPatterns(
    MachineInstr &Root, SmallVectorImpl<unsigned> &Patterns,
    bool DoRegPressureReduce) const {

  if (getFPPatterns(Root, Patterns, DoRegPressureReduce))
    return true;

  if (getSHXADDPatterns(Root, Patterns))
    return true;

  return TargetInstrInfo::getMachineCombinerPatterns(Root, Patterns,
                                                     DoRegPressureReduce);
}

static unsigned getFPFusedMultiplyOpcode(unsigned RootOpc, unsigned Pattern) {
  switch (RootOpc) {
  default:
    llvm_unreachable("Unexpected opcode");
  case RVTrim::FADD_H:
    return RVTrim::FMADD_H;
  case RVTrim::FADD_S:
    return RVTrim::FMADD_S;
  case RVTrim::FADD_D:
    return RVTrim::FMADD_D;
  case RVTrim::FSUB_H:
    return Pattern == RVTrimMachineCombinerPattern::FMSUB ? RVTrim::FMSUB_H
                                                         : RVTrim::FNMSUB_H;
  case RVTrim::FSUB_S:
    return Pattern == RVTrimMachineCombinerPattern::FMSUB ? RVTrim::FMSUB_S
                                                         : RVTrim::FNMSUB_S;
  case RVTrim::FSUB_D:
    return Pattern == RVTrimMachineCombinerPattern::FMSUB ? RVTrim::FMSUB_D
                                                         : RVTrim::FNMSUB_D;
  }
}

static unsigned getAddendOperandIdx(unsigned Pattern) {
  switch (Pattern) {
  default:
    llvm_unreachable("Unexpected pattern");
  case RVTrimMachineCombinerPattern::FMADD_AX:
  case RVTrimMachineCombinerPattern::FMSUB:
    return 2;
  case RVTrimMachineCombinerPattern::FMADD_XA:
  case RVTrimMachineCombinerPattern::FNMSUB:
    return 1;
  }
}

static void combineFPFusedMultiply(MachineInstr &Root, MachineInstr &Prev,
                                   unsigned Pattern,
                                   SmallVectorImpl<MachineInstr *> &InsInstrs,
                                   SmallVectorImpl<MachineInstr *> &DelInstrs) {
  MachineFunction *MF = Root.getMF();
  MachineRegisterInfo &MRI = MF->getRegInfo();
  const TargetInstrInfo *TII = MF->getSubtarget().getInstrInfo();

  MachineOperand &Mul1 = Prev.getOperand(1);
  MachineOperand &Mul2 = Prev.getOperand(2);
  MachineOperand &Dst = Root.getOperand(0);
  MachineOperand &Addend = Root.getOperand(getAddendOperandIdx(Pattern));

  Register DstReg = Dst.getReg();
  unsigned FusedOpc = getFPFusedMultiplyOpcode(Root.getOpcode(), Pattern);
  uint32_t IntersectedFlags = Root.getFlags() & Prev.getFlags();
  DebugLoc MergedLoc =
      DILocation::getMergedLocation(Root.getDebugLoc(), Prev.getDebugLoc());

  // We need to clear kill flags since we may be extending the live range past
  // a kill.
  MRI.clearKillFlags(Mul1.getReg());
  MRI.clearKillFlags(Mul2.getReg());

  MachineInstrBuilder MIB = BuildMI(*MF, MergedLoc, TII->get(FusedOpc), DstReg)
                                .addReg(Mul1.getReg())
                                .addReg(Mul2.getReg())
                                .addReg(Addend.getReg())
                                .setMIFlags(IntersectedFlags);

  InsInstrs.push_back(MIB);
  if (MRI.hasOneNonDBGUse(Prev.getOperand(0).getReg()))
    DelInstrs.push_back(&Prev);
  DelInstrs.push_back(&Root);
}

// Combine patterns like (sh3add Z, (add X, (slli Y, 5))) to
// (sh3add (sh2add Y, Z), X) if the shift amount can be split across two
// shXadd instructions. The outer shXadd keeps its original opcode.
static void
genShXAddAddShift(MachineInstr &Root, unsigned AddOpIdx,
                  SmallVectorImpl<MachineInstr *> &InsInstrs,
                  SmallVectorImpl<MachineInstr *> &DelInstrs,
                  DenseMap<Register, unsigned> &InstrIdxForVirtReg) {
  MachineFunction *MF = Root.getMF();
  MachineRegisterInfo &MRI = MF->getRegInfo();
  const TargetInstrInfo *TII = MF->getSubtarget().getInstrInfo();

  unsigned OuterShiftAmt = getSHXADDShiftAmount(Root.getOpcode());
  assert(OuterShiftAmt != 0 && "Unexpected opcode");

  MachineInstr *AddMI = MRI.getUniqueVRegDef(Root.getOperand(2).getReg());
  MachineInstr *ShiftMI =
      MRI.getUniqueVRegDef(AddMI->getOperand(AddOpIdx).getReg());

  unsigned InnerShiftAmt = ShiftMI->getOperand(2).getImm();
  assert(InnerShiftAmt >= OuterShiftAmt && "Unexpected shift amount");

  unsigned InnerOpc;
  switch (InnerShiftAmt - OuterShiftAmt) {
  default:
    llvm_unreachable("Unexpected shift amount");
  case 0:
    InnerOpc = RVTrim::ADD;
    break;
  case 1:
    InnerOpc = RVTrim::SH1ADD;
    break;
  case 2:
    InnerOpc = RVTrim::SH2ADD;
    break;
  case 3:
    InnerOpc = RVTrim::SH3ADD;
    break;
  }

  const MachineOperand &X = AddMI->getOperand(3 - AddOpIdx);
  const MachineOperand &Y = ShiftMI->getOperand(1);
  const MachineOperand &Z = Root.getOperand(1);

  Register NewVR = MRI.createVirtualRegister(&RVTrim::GPRRegClass);

  auto MIB1 = BuildMI(*MF, MIMetadata(Root), TII->get(InnerOpc), NewVR)
                  .addReg(Y.getReg())
                  .addReg(Z.getReg());
  auto MIB2 = BuildMI(*MF, MIMetadata(Root), TII->get(Root.getOpcode()),
                      Root.getOperand(0).getReg())
                  .addReg(NewVR)
                  .addReg(X.getReg());

  InstrIdxForVirtReg.insert(std::make_pair(NewVR, 0));
  InsInstrs.push_back(MIB1);
  InsInstrs.push_back(MIB2);
  DelInstrs.push_back(ShiftMI);
  DelInstrs.push_back(AddMI);
  DelInstrs.push_back(&Root);
}

void RVTrimInstrInfo::genAlternativeCodeSequence(
    MachineInstr &Root, unsigned Pattern,
    SmallVectorImpl<MachineInstr *> &InsInstrs,
    SmallVectorImpl<MachineInstr *> &DelInstrs,
    DenseMap<Register, unsigned> &InstrIdxForVirtReg) const {
  MachineRegisterInfo &MRI = Root.getMF()->getRegInfo();
  switch (Pattern) {
  default:
    TargetInstrInfo::genAlternativeCodeSequence(Root, Pattern, InsInstrs,
                                                DelInstrs, InstrIdxForVirtReg);
    return;
  case RVTrimMachineCombinerPattern::FMADD_AX:
  case RVTrimMachineCombinerPattern::FMSUB: {
    MachineInstr &Prev = *MRI.getVRegDef(Root.getOperand(1).getReg());
    combineFPFusedMultiply(Root, Prev, Pattern, InsInstrs, DelInstrs);
    return;
  }
  case RVTrimMachineCombinerPattern::FMADD_XA:
  case RVTrimMachineCombinerPattern::FNMSUB: {
    MachineInstr &Prev = *MRI.getVRegDef(Root.getOperand(2).getReg());
    combineFPFusedMultiply(Root, Prev, Pattern, InsInstrs, DelInstrs);
    return;
  }
  case RVTrimMachineCombinerPattern::SHXADD_ADD_SLLI_OP1:
    genShXAddAddShift(Root, 1, InsInstrs, DelInstrs, InstrIdxForVirtReg);
    return;
  case RVTrimMachineCombinerPattern::SHXADD_ADD_SLLI_OP2:
    genShXAddAddShift(Root, 2, InsInstrs, DelInstrs, InstrIdxForVirtReg);
    return;
  }
}

bool RVTrimInstrInfo::verifyInstruction(const MachineInstr &MI,
                                       StringRef &ErrInfo) const {
  MCInstrDesc const &Desc = MI.getDesc();

  for (const auto &[Index, Operand] : enumerate(Desc.operands())) {
    const MachineOperand &MO = MI.getOperand(Index);
    unsigned OpType = Operand.OperandType;
    switch (OpType) {
    default:
      if (OpType >= RVTrimOp::OPERAND_FIRST_RVTrim_IMM &&
          OpType <= RVTrimOp::OPERAND_LAST_RVTrim_IMM) {
        if (!MO.isImm()) {
          ErrInfo = "Expected an immediate operand.";
          return false;
        }
        int64_t Imm = MO.getImm();
        bool Ok;
        switch (OpType) {
        default:
          llvm_unreachable("Unexpected operand type");

#define CASE_OPERAND_UIMM(NUM)                                                 \
  case RVTrimOp::OPERAND_UIMM##NUM:                                             \
    Ok = isUInt<NUM>(Imm);                                                     \
    break;
#define CASE_OPERAND_UIMM_LSB_ZEROS(BITS, SUFFIX)                              \
  case RVTrimOp::OPERAND_UIMM##BITS##_LSB##SUFFIX: {                            \
    constexpr size_t NumZeros = sizeof(#SUFFIX) - 1;                           \
    Ok = isShiftedUInt<BITS - NumZeros, NumZeros>(Imm);                        \
    break;                                                                     \
  }
#define CASE_OPERAND_SIMM(NUM)                                                 \
  case RVTrimOp::OPERAND_SIMM##NUM:                                             \
    Ok = isInt<NUM>(Imm);                                                      \
    break;
          // clang-format off
        CASE_OPERAND_UIMM(1)
        CASE_OPERAND_UIMM(2)
        CASE_OPERAND_UIMM(3)
        CASE_OPERAND_UIMM(4)
        CASE_OPERAND_UIMM(5)
        CASE_OPERAND_UIMM(6)
        CASE_OPERAND_UIMM(7)
        CASE_OPERAND_UIMM(8)
        CASE_OPERAND_UIMM(9)
        CASE_OPERAND_UIMM(10)
        CASE_OPERAND_UIMM(12)
        CASE_OPERAND_UIMM(16)
        CASE_OPERAND_UIMM(32)
        CASE_OPERAND_UIMM(48)
        CASE_OPERAND_UIMM(64)
        CASE_OPERAND_UIMM_LSB_ZEROS(2, 0)
        CASE_OPERAND_UIMM_LSB_ZEROS(5, 0)
        CASE_OPERAND_UIMM_LSB_ZEROS(6, 0)
        CASE_OPERAND_UIMM_LSB_ZEROS(6, 000)
        CASE_OPERAND_UIMM_LSB_ZEROS(7, 00)
        CASE_OPERAND_UIMM_LSB_ZEROS(7, 000)
        CASE_OPERAND_UIMM_LSB_ZEROS(8, 00)
        CASE_OPERAND_UIMM_LSB_ZEROS(8, 000)
        CASE_OPERAND_UIMM_LSB_ZEROS(9, 000)
        // clang-format on
        case RVTrimOp::OPERAND_UIMM4_PLUS1:
          Ok = Imm >= 1 && Imm <= 16;
          break;
        case RVTrimOp::OPERAND_UIMM5_NONZERO:
          Ok = isUInt<5>(Imm) && (Imm != 0);
          break;
        case RVTrimOp::OPERAND_UIMM5_GT3:
          Ok = isUInt<5>(Imm) && (Imm > 3);
          break;
        case RVTrimOp::OPERAND_UIMM5_PLUS1:
          Ok = Imm >= 1 && Imm <= 32;
          break;
        case RVTrimOp::OPERAND_UIMM6_PLUS1:
          Ok = Imm >= 1 && Imm <= 64;
          break;
        case RVTrimOp::OPERAND_UIMM8_GE32:
          Ok = isUInt<8>(Imm) && Imm >= 32;
          break;
        case RVTrimOp::OPERAND_UIMM9_YBNDSWI:
          Ok = RVTrim::isValidYBNDSWImm(Imm);
          break;
        case RVTrimOp::OPERAND_SIMM10_LSB0000_NONZERO:
          Ok = isShiftedInt<6, 4>(Imm) && (Imm != 0);
          break;
        case RVTrimOp::OPERAND_UIMM10_LSB00_NONZERO:
          Ok = isShiftedUInt<8, 2>(Imm) && (Imm != 0);
          break;
        case RVTrimOp::OPERAND_UIMM16_NONZERO:
          Ok = isUInt<16>(Imm) && (Imm != 0);
          break;
        case RVTrimOp::OPERAND_THREE:
          Ok = Imm == 3;
          break;
        case RVTrimOp::OPERAND_FOUR:
          Ok = Imm == 4;
          break;
        case RVTrimOp::OPERAND_IMM5_ZIBI:
          Ok = (isUInt<5>(Imm) && Imm != 0) || Imm == -1;
          break;
          // clang-format off
        CASE_OPERAND_SIMM(5)
        CASE_OPERAND_SIMM(6)
        CASE_OPERAND_SIMM(8)
        CASE_OPERAND_SIMM(10)
        CASE_OPERAND_SIMM(11)
        CASE_OPERAND_SIMM(12)
        CASE_OPERAND_SIMM(26)
        // clang-format on
        case RVTrimOp::OPERAND_SIMM5_PLUS1:
          Ok = Imm >= -15 && Imm <= 16;
          break;
        case RVTrimOp::OPERAND_SIMM5_NONZERO:
          Ok = isInt<5>(Imm) && (Imm != 0);
          break;
        case RVTrimOp::OPERAND_SIMM6_NONZERO:
          Ok = Imm != 0 && isInt<6>(Imm);
          break;
        case RVTrimOp::OPERAND_VTYPEI10:
          Ok = isUInt<10>(Imm) && RISCVVType::isValidVType(Imm);
          break;
        case RVTrimOp::OPERAND_VTYPEI11:
          Ok = isUInt<11>(Imm) && RISCVVType::isValidVType(Imm);
          break;
        case RVTrimOp::OPERAND_SIMM12_LSB00000:
          Ok = isShiftedInt<7, 5>(Imm);
          break;
        case RVTrimOp::OPERAND_SIMM16_NONZERO:
          Ok = isInt<16>(Imm) && (Imm != 0);
          break;
        case RVTrimOp::OPERAND_SIMM20_LI:
          Ok = isInt<20>(Imm);
          break;
        case RVTrimOp::OPERAND_UIMMLOG2XLEN:
          Ok = STI.is64Bit() ? isUInt<6>(Imm) : isUInt<5>(Imm);
          break;
        case RVTrimOp::OPERAND_UIMMLOG2XLEN_NONZERO:
          Ok = STI.is64Bit() ? isUInt<6>(Imm) : isUInt<5>(Imm);
          Ok = Ok && Imm != 0;
          break;
        case RVTrimOp::OPERAND_CLUI_IMM:
          Ok = (isUInt<5>(Imm) && Imm != 0) || (Imm >= 0xfffe0 && Imm <= 0xfffff);
          break;
        case RVTrimOp::OPERAND_RVKRNUM:
          Ok = Imm >= 0 && Imm <= 10;
          break;
        case RVTrimOp::OPERAND_RVKRNUM_0_7:
          Ok = Imm >= 0 && Imm <= 7;
          break;
        case RVTrimOp::OPERAND_RVKRNUM_1_10:
          Ok = Imm >= 1 && Imm <= 10;
          break;
        case RVTrimOp::OPERAND_RVKRNUM_2_14:
          Ok = Imm >= 2 && Imm <= 14;
          break;
        case RVTrimOp::OPERAND_RLIST:
          Ok = Imm >= RVTrimZC::RA && Imm <= RVTrimZC::RA_S0_S11;
          break;
        case RVTrimOp::OPERAND_RLIST_S0:
          Ok = Imm >= RVTrimZC::RA_S0 && Imm <= RVTrimZC::RA_S0_S11;
          break;
        case RVTrimOp::OPERAND_STACKADJ:
          Ok = Imm >= 0 && Imm <= 48 && Imm % 16 == 0;
          break;
        case RVTrimOp::OPERAND_FRMARG:
          Ok = RVTrimFPRndMode::isValidRoundingMode(Imm);
          break;
        case RVTrimOp::OPERAND_RTZARG:
          Ok = Imm == RVTrimFPRndMode::RTZ;
          break;
        case RVTrimOp::OPERAND_SMTVType:
          Ok = XSMTVTypeMode::isValidSMTVTypeMode(Imm);
          break;
        case RVTrimOp::OPERAND_SMTI8:
          Ok = Imm == XSMTVTypeMode::SMT_I8;
          break;
        case RVTrimOp::OPERAND_COND_CODE:
          Ok = Imm >= 0 && Imm < RVTrimCC::COND_INVALID;
          break;
        case RVTrimOp::OPERAND_ATOMIC_ORDERING:
          Ok = isValidAtomicOrdering(Imm);
          break;
        case RVTrimOp::OPERAND_VEC_POLICY:
          Ok = (Imm & (RISCVVType::TAIL_AGNOSTIC | RISCVVType::MASK_AGNOSTIC)) ==
               Imm;
          break;
        case RVTrimOp::OPERAND_SEW:
          Ok = (isUInt<5>(Imm) && RISCVVType::isValidSEW(1 << Imm));
          break;
        case RVTrimOp::OPERAND_SEW_MASK:
          Ok = Imm == 0;
          break;
        case RVTrimOp::OPERAND_VEC_RM:
          assert(RVTrimII::hasRoundModeOp(Desc.TSFlags));
          if (RVTrimII::usesVXRM(Desc.TSFlags))
            Ok = isUInt<2>(Imm);
          else
            Ok = RVTrimFPRndMode::isValidRoundingMode(Imm);
          break;
        case RVTrimOp::OPERAND_XSFMM_VTYPE:
          Ok = RISCVVType::isValidXSfmmVType(Imm);
          break;
        case RVTrimOp::OPERAND_XSFMM_TWIDEN:
          Ok = Imm == 1 || Imm == 2 || Imm == 4;
          break;
        }
        if (!Ok) {
          ErrInfo = "Invalid immediate";
          return false;
        }
      }
      break;
    case RVTrimOp::OPERAND_SIMM12_LO:
      // TODO: We could be stricter about what non-register operands are
      // allowed.
      if (MO.isReg()) {
        ErrInfo = "Expected a non-register operand.";
        return false;
      }
      if (MO.isImm() && !isInt<12>(MO.getImm())) {
        ErrInfo = "Invalid immediate";
        return false;
      }
      break;
    case RVTrimOp::OPERAND_UIMM20_LUI:
    case RVTrimOp::OPERAND_UIMM20_AUIPC:
      // TODO: We could be stricter about what non-register operands are
      // allowed.
      if (MO.isReg()) {
        ErrInfo = "Expected a non-register operand.";
        return false;
      }
      if (MO.isImm() && !isUInt<20>(MO.getImm())) {
        ErrInfo = "Invalid immediate";
        return false;
      }
      break;
    case RVTrimOp::OPERAND_BARE_SIMM32:
      // TODO: We could be stricter about what non-register operands are
      // allowed.
      if (MO.isReg()) {
        ErrInfo = "Expected a non-register operand.";
        return false;
      }
      if (MO.isImm() && !isInt<32>(MO.getImm())) {
        ErrInfo = "Invalid immediate";
        return false;
      }
      break;
    case RVTrimOp::OPERAND_AVL:
      if (MO.isImm()) {
        int64_t Imm = MO.getImm();
        // VLMAX is represented as -1.
        if (!isUInt<5>(Imm) && Imm != -1) {
          ErrInfo = "Invalid immediate";
          return false;
        }
      } else if (!MO.isReg()) {
        ErrInfo = "Expected a register or immediate operand.";
        return false;
      }
      break;
    case RVTrimOp::OPERAND_SFB_RHS:
      if (!MO.isReg() && !MO.isImm()) {
        ErrInfo = "Expected a register or immediate operand.";
        return false;
      }
      break;
    }
  }

  const uint64_t TSFlags = Desc.TSFlags;
  if (RVTrimII::hasVLOp(TSFlags)) {
    const MachineOperand &Op = MI.getOperand(RVTrimII::getVLOpNum(Desc));
    if (!Op.isImm() && !Op.isReg())  {
      ErrInfo = "Invalid operand type for VL operand";
      return false;
    }
    if (Op.isReg() && Op.getReg().isValid()) {
      const MachineRegisterInfo &MRI = MI.getParent()->getParent()->getRegInfo();
      auto *RC = MRI.getRegClass(Op.getReg());
      if (!RVTrim::GPRNoX0RegClass.hasSubClassEq(RC)) {
        ErrInfo = "Invalid register class for VL operand";
        return false;
      }
    }
    if (!RVTrimII::hasSEWOp(TSFlags)) {
      ErrInfo = "VL operand w/o SEW operand?";
      return false;
    }
  }
  if (RVTrimII::hasSEWOp(TSFlags)) {
    unsigned OpIdx = RVTrimII::getSEWOpNum(Desc);
    if (!MI.getOperand(OpIdx).isImm()) {
      ErrInfo = "SEW value expected to be an immediate";
      return false;
    }
    uint64_t Log2SEW = MI.getOperand(OpIdx).getImm();
    if (Log2SEW > 31) {
      ErrInfo = "Unexpected SEW value";
      return false;
    }
    unsigned SEW = Log2SEW ? 1 << Log2SEW : 8;
    if (!RISCVVType::isValidSEW(SEW)) {
      ErrInfo = "Unexpected SEW value";
      return false;
    }
  }
  if (RVTrimII::hasVecPolicyOp(TSFlags)) {
    unsigned OpIdx = RVTrimII::getVecPolicyOpNum(Desc);
    if (!MI.getOperand(OpIdx).isImm()) {
      ErrInfo = "Policy operand expected to be an immediate";
      return false;
    }
    uint64_t Policy = MI.getOperand(OpIdx).getImm();
    if (Policy > (RISCVVType::TAIL_AGNOSTIC | RISCVVType::MASK_AGNOSTIC)) {
      ErrInfo = "Invalid Policy Value";
      return false;
    }
    if (!RVTrimII::hasVLOp(TSFlags)) {
      ErrInfo = "policy operand w/o VL operand?";
      return false;
    }

    // VecPolicy operands can only exist on instructions with passthru/merge
    // arguments. Note that not all arguments with passthru have vec policy
    // operands- some instructions have implicit policies.
    unsigned UseOpIdx;
    if (!MI.isRegTiedToUseOperand(0, &UseOpIdx)) {
      ErrInfo = "policy operand w/o tied operand?";
      return false;
    }
  }

  if (int Idx = RVTrimII::getFRMOpNum(Desc);
      Idx >= 0 && MI.getOperand(Idx).getImm() == RVTrimFPRndMode::DYN &&
      !MI.readsRegister(RVTrim::FRM, /*TRI=*/nullptr)) {
    ErrInfo = "dynamic rounding mode should read FRM";
    return false;
  }

  return true;
}

bool RVTrimInstrInfo::canFoldIntoAddrMode(const MachineInstr &MemI, Register Reg,
                                         const MachineInstr &AddrI,
                                         ExtAddrMode &AM) const {
  switch (MemI.getOpcode()) {
  default:
    return false;
  case RVTrim::LB:
  case RVTrim::LBU:
  case RVTrim::LH:
  case RVTrim::LH_INX:
  case RVTrim::LHU:
  case RVTrim::LW:
  case RVTrim::LW_INX:
  case RVTrim::LWU:
  case RVTrim::LD:
  case RVTrim::LD_RV32:
  case RVTrim::FLH:
  case RVTrim::FLW:
  case RVTrim::FLD:
  case RVTrim::SB:
  case RVTrim::SH:
  case RVTrim::SH_INX:
  case RVTrim::SW:
  case RVTrim::SW_INX:
  case RVTrim::SD:
  case RVTrim::SD_RV32:
  case RVTrim::FSH:
  case RVTrim::FSW:
  case RVTrim::FSD:
    break;
  }

  if (MemI.getOperand(0).getReg() == Reg)
    return false;

  if (AddrI.getOpcode() != RVTrim::ADDI || !AddrI.getOperand(1).isReg() ||
      !AddrI.getOperand(2).isImm())
    return false;

  int64_t OldOffset = MemI.getOperand(2).getImm();
  int64_t Disp = AddrI.getOperand(2).getImm();
  int64_t NewOffset = OldOffset + Disp;
  if (!STI.is64Bit())
    NewOffset = SignExtend64<32>(NewOffset);

  if (!isInt<12>(NewOffset))
    return false;

  AM.BaseReg = AddrI.getOperand(1).getReg();
  AM.ScaledReg = 0;
  AM.Scale = 0;
  AM.Displacement = NewOffset;
  AM.Form = ExtAddrMode::Formula::Basic;
  return true;
}

MachineInstr *RVTrimInstrInfo::emitLdStWithAddr(MachineInstr &MemI,
                                               const ExtAddrMode &AM) const {

  const DebugLoc &DL = MemI.getDebugLoc();
  MachineBasicBlock &MBB = *MemI.getParent();

  assert(AM.ScaledReg == 0 && AM.Scale == 0 &&
         "Addressing mode not supported for folding");

  return BuildMI(MBB, MemI, DL, get(MemI.getOpcode()))
      .addReg(MemI.getOperand(0).getReg(), getDefRegState(MemI.mayLoad()))
      .addReg(AM.BaseReg)
      .addImm(AM.Displacement)
      .setMemRefs(MemI.memoperands())
      .setMIFlags(MemI.getFlags());
}

// TODO: At the moment, MIPS introduced paring of instructions operating with
// word or double word. This should be extended with more instructions when more
// vendors support load/store pairing.
bool RVTrimInstrInfo::isPairableLdStInstOpc(unsigned Opc) {
  switch (Opc) {
  default:
    return false;
  case RVTrim::SW:
  case RVTrim::SD:
  case RVTrim::LD:
  case RVTrim::LW:
    return true;
  }
}

bool RVTrimInstrInfo::isLdStSafeToPair(const MachineInstr &LdSt,
                                      const TargetRegisterInfo *TRI) {
  // If this is a volatile load/store, don't mess with it.
  if (LdSt.hasOrderedMemoryRef() || LdSt.getNumExplicitOperands() != 3)
    return false;

  if (LdSt.getOperand(1).isFI())
    return true;

  assert(LdSt.getOperand(1).isReg() && "Expected a reg operand.");
  // Can't cluster if the instruction modifies the base register
  // or it is update form. e.g. ld x5,8(x5)
  if (LdSt.modifiesRegister(LdSt.getOperand(1).getReg(), TRI))
    return false;

  if (!LdSt.getOperand(2).isImm())
    return false;

  return true;
}

bool RVTrimInstrInfo::getMemOperandsWithOffsetWidth(
    const MachineInstr &LdSt, SmallVectorImpl<const MachineOperand *> &BaseOps,
    int64_t &Offset, bool &OffsetIsScalable, LocationSize &Width) const {
  if (!LdSt.mayLoadOrStore())
    return false;

  // Conservatively, only handle scalar loads/stores for now.
  switch (LdSt.getOpcode()) {
  case RVTrim::LB:
  case RVTrim::LBU:
  case RVTrim::SB:
  case RVTrim::LH:
  case RVTrim::LH_INX:
  case RVTrim::LHU:
  case RVTrim::FLH:
  case RVTrim::SH:
  case RVTrim::SH_INX:
  case RVTrim::FSH:
  case RVTrim::LW:
  case RVTrim::LW_INX:
  case RVTrim::LWU:
  case RVTrim::FLW:
  case RVTrim::SW:
  case RVTrim::SW_INX:
  case RVTrim::FSW:
  case RVTrim::LD:
  case RVTrim::LD_RV32:
  case RVTrim::FLD:
  case RVTrim::SD:
  case RVTrim::SD_RV32:
  case RVTrim::FSD:
    break;
  default:
    return false;
  }
  const MachineOperand *BaseOp;
  OffsetIsScalable = false;
  if (!getMemOperandWithOffsetWidth(LdSt, BaseOp, Offset, Width))
    return false;
  BaseOps.push_back(BaseOp);
  return true;
}

// TODO: This was copied from SIInstrInfo. Could it be lifted to a common
// helper?
static bool memOpsHaveSameBasePtr(const MachineInstr &MI1,
                                  ArrayRef<const MachineOperand *> BaseOps1,
                                  const MachineInstr &MI2,
                                  ArrayRef<const MachineOperand *> BaseOps2) {
  // Only examine the first "base" operand of each instruction, on the
  // assumption that it represents the real base address of the memory access.
  // Other operands are typically offsets or indices from this base address.
  if (BaseOps1.front()->isIdenticalTo(*BaseOps2.front()))
    return true;

  if (!MI1.hasOneMemOperand() || !MI2.hasOneMemOperand())
    return false;

  auto MO1 = *MI1.memoperands_begin();
  auto MO2 = *MI2.memoperands_begin();
  if (MO1->getAddrSpace() != MO2->getAddrSpace())
    return false;

  auto Base1 = MO1->getValue();
  auto Base2 = MO2->getValue();
  if (!Base1 || !Base2)
    return false;
  Base1 = getUnderlyingObject(Base1);
  Base2 = getUnderlyingObject(Base2);

  if (isa<UndefValue>(Base1) || isa<UndefValue>(Base2))
    return false;

  return Base1 == Base2;
}

bool RVTrimInstrInfo::shouldClusterMemOps(
    ArrayRef<const MachineOperand *> BaseOps1, int64_t Offset1,
    bool OffsetIsScalable1, ArrayRef<const MachineOperand *> BaseOps2,
    int64_t Offset2, bool OffsetIsScalable2, unsigned ClusterSize,
    unsigned NumBytes) const {
  // If the mem ops (to be clustered) do not have the same base ptr, then they
  // should not be clustered
  if (!BaseOps1.empty() && !BaseOps2.empty()) {
    const MachineInstr &FirstLdSt = *BaseOps1.front()->getParent();
    const MachineInstr &SecondLdSt = *BaseOps2.front()->getParent();
    if (!memOpsHaveSameBasePtr(FirstLdSt, BaseOps1, SecondLdSt, BaseOps2))
      return false;
  } else if (!BaseOps1.empty() || !BaseOps2.empty()) {
    // If only one base op is empty, they do not have the same base ptr
    return false;
  }

  unsigned CacheLineSize =
      BaseOps1.front()->getParent()->getMF()->getSubtarget().getCacheLineSize();
  // Assume a cache line size of 64 bytes if no size is set in RVTrimSubtarget.
  CacheLineSize = CacheLineSize ? CacheLineSize : 64;
  // Cluster if the memory operations are on the same or a neighbouring cache
  // line, but limit the maximum ClusterSize to avoid creating too much
  // additional register pressure.
  return ClusterSize <= 4 && std::abs(Offset1 - Offset2) < CacheLineSize;
}

// Set BaseReg (the base register operand), Offset (the byte offset being
// accessed) and the access Width of the passed instruction that reads/writes
// memory. Returns false if the instruction does not read/write memory or the
// BaseReg/Offset/Width can't be determined. Is not guaranteed to always
// recognise base operands and offsets in all cases.
// TODO: Add an IsScalable bool ref argument (like the equivalent AArch64
// function) and set it as appropriate.
bool RVTrimInstrInfo::getMemOperandWithOffsetWidth(
    const MachineInstr &LdSt, const MachineOperand *&BaseReg, int64_t &Offset,
    LocationSize &Width) const {
  if (!LdSt.mayLoadOrStore())
    return false;

  // Here we assume the standard RISC-V ISA, which uses a base+offset
  // addressing mode. You'll need to relax these conditions to support custom
  // load/store instructions.
  if (LdSt.getNumExplicitOperands() != 3)
    return false;
  if ((!LdSt.getOperand(1).isReg() && !LdSt.getOperand(1).isFI()) ||
      !LdSt.getOperand(2).isImm())
    return false;

  if (!LdSt.hasOneMemOperand())
    return false;

  Width = (*LdSt.memoperands_begin())->getSize();
  BaseReg = &LdSt.getOperand(1);
  Offset = LdSt.getOperand(2).getImm();
  return true;
}

bool RVTrimInstrInfo::areMemAccessesTriviallyDisjoint(
    const MachineInstr &MIa, const MachineInstr &MIb) const {
  assert(MIa.mayLoadOrStore() && "MIa must be a load or store.");
  assert(MIb.mayLoadOrStore() && "MIb must be a load or store.");

  if (MIa.hasUnmodeledSideEffects() || MIb.hasUnmodeledSideEffects() ||
      MIa.hasOrderedMemoryRef() || MIb.hasOrderedMemoryRef())
    return false;

  // Retrieve the base register, offset from the base register and width. Width
  // is the size of memory that is being loaded/stored (e.g. 1, 2, 4).  If
  // base registers are identical, and the offset of a lower memory access +
  // the width doesn't overlap the offset of a higher memory access,
  // then the memory accesses are different.
  const MachineOperand *BaseOpA = nullptr, *BaseOpB = nullptr;
  int64_t OffsetA = 0, OffsetB = 0;
  LocationSize WidthA = LocationSize::precise(0),
               WidthB = LocationSize::precise(0);
  if (getMemOperandWithOffsetWidth(MIa, BaseOpA, OffsetA, WidthA) &&
      getMemOperandWithOffsetWidth(MIb, BaseOpB, OffsetB, WidthB)) {
    if (BaseOpA->isIdenticalTo(*BaseOpB)) {
      int LowOffset = std::min(OffsetA, OffsetB);
      int HighOffset = std::max(OffsetA, OffsetB);
      LocationSize LowWidth = (LowOffset == OffsetA) ? WidthA : WidthB;
      if (LowWidth.hasValue() &&
          LowOffset + (int)LowWidth.getValue() <= HighOffset)
        return true;
    }
  }
  return false;
}

std::pair<unsigned, unsigned>
RVTrimInstrInfo::decomposeMachineOperandsTargetFlags(unsigned TF) const {
  const unsigned Mask = RVTrimII::MO_DIRECT_FLAG_MASK;
  return std::make_pair(TF & Mask, TF & ~Mask);
}

ArrayRef<std::pair<unsigned, const char *>>
RVTrimInstrInfo::getSerializableDirectMachineOperandTargetFlags() const {
  using namespace RVTrimII;
  static const std::pair<unsigned, const char *> TargetFlags[] = {
      {MO_CALL, "rvtrim-call"},
      {MO_LO, "rvtrim-lo"},
      {MO_HI, "rvtrim-hi"},
      {MO_PCREL_LO, "rvtrim-pcrel-lo"},
      {MO_PCREL_HI, "rvtrim-pcrel-hi"},
      {MO_GOT_HI, "rvtrim-got-hi"},
      {MO_TPREL_LO, "rvtrim-tprel-lo"},
      {MO_TPREL_HI, "rvtrim-tprel-hi"},
      {MO_TPREL_ADD, "rvtrim-tprel-add"},
      {MO_TLS_GOT_HI, "rvtrim-tls-got-hi"},
      {MO_TLS_GD_HI, "rvtrim-tls-gd-hi"},
      {MO_TLSDESC_HI, "rvtrim-tlsdesc-hi"},
      {MO_TLSDESC_LOAD_LO, "rvtrim-tlsdesc-load-lo"},
      {MO_TLSDESC_ADD_LO, "rvtrim-tlsdesc-add-lo"},
      {MO_TLSDESC_CALL, "rvtrim-tlsdesc-call"},
      {MO_QC_ACCESS, "rvtrim-qc-access"},
  };
  return ArrayRef(TargetFlags);
}

bool RVTrimInstrInfo::isMBBSafeToOutlineFrom(MachineBasicBlock &MBB,
                                            unsigned &Flags) const {
  // More accurate safety checking is done in getOutliningCandidateInfo.
  return TargetInstrInfo::isMBBSafeToOutlineFrom(MBB, Flags);
}

// Enum values indicating how an outlined call should be constructed.
enum MachineOutlinerConstructionID {
  MachineOutlinerTailCall,
  MachineOutlinerDefault,
  MachineOutlinerRegSave
};

bool RVTrimInstrInfo::shouldOutlineFromFunctionByDefault(
    MachineFunction &MF) const {
  return MF.getFunction().hasMinSize();
}

static bool isCandidatePatchable(const MachineBasicBlock &MBB) {
  const MachineFunction *MF = MBB.getParent();
  const Function &F = MF->getFunction();
  return F.getFnAttribute("fentry-call").getValueAsBool() ||
         F.hasFnAttribute("patchable-function-entry");
}

static bool isMIReadsReg(const MachineInstr &MI, const TargetRegisterInfo *TRI,
                         MCRegister RegNo) {
  return MI.readsRegister(RegNo, TRI) ||
         MI.getDesc().hasImplicitUseOfPhysReg(RegNo);
}

static bool isMIModifiesReg(const MachineInstr &MI,
                            const TargetRegisterInfo *TRI, MCRegister RegNo) {
  return MI.modifiesRegister(RegNo, TRI) ||
         MI.getDesc().hasImplicitDefOfPhysReg(RegNo);
}

static bool cannotInsertTailCall(const MachineBasicBlock &MBB) {
  if (!MBB.back().isReturn())
    return true;
  if (isCandidatePatchable(MBB))
    return true;

  // If the candidate reads the pre-set register
  // that can be used for expanding PseudoTAIL instruction,
  // then we cannot insert tail call.
  const TargetSubtargetInfo &STI = MBB.getParent()->getSubtarget();
  const RVTrimMachineFunctionInfo *RVFI =
      MBB.getParent()->getInfo<RVTrimMachineFunctionInfo>();
  // When cf-protection-branch is active, the outliner will emit PseudoTAILX7
  // which always uses X7. Otherwise, PseudoTAIL is emitted and the register
  // is determined by Zicfilp at encode time.
  MCRegister TailExpandUseRegNo =
      RVFI->hasCFProtectionBranch()
          ? RVTrim::X7
          : RVTrimII::getTailExpandUseRegNo(STI.getFeatureBits());
  for (const MachineInstr &MI : MBB) {
    if (isMIReadsReg(MI, STI.getRegisterInfo(), TailExpandUseRegNo))
      return true;
    if (isMIModifiesReg(MI, STI.getRegisterInfo(), TailExpandUseRegNo))
      break;
  }
  return false;
}

static Register findRegisterToSaveX5To(outliner::Candidate &C,
                                       const TargetRegisterInfo &TRI) {
  // Candidate registers for saving X5: t1-t6
  static const MCPhysReg TempRegs[] = {
      RVTrim::X6,  // t1
      RVTrim::X7,  // t2
      RVTrim::X28, // t3
      RVTrim::X29, // t4
      RVTrim::X30, // t5
      RVTrim::X31  // t6
  };

  const MachineFunction *MF = C.getMF();
  const MachineRegisterInfo &MRI = MF->getRegInfo();

  for (MCPhysReg Reg : TempRegs) {
    if (MRI.isReserved(Reg))
      continue;

    if (C.isAvailableAcrossAndOutOfSeq(Reg, TRI) &&
        C.isAvailableInsideSeq(Reg, TRI)) {
      return Reg;
    }
  }

  return Register();
}

bool RVTrimInstrInfo::analyzeCandidate(outliner::Candidate &C) const {
  // If the expansion register for tail calls is live across the candidate
  // outlined call site, we cannot outline that candidate as the expansion
  // would clobber the register.
  const RVTrimMachineFunctionInfo *RVFI =
      C.getMF()->getInfo<RVTrimMachineFunctionInfo>();
  MCRegister TailExpandUseReg =
      RVFI->hasCFProtectionBranch()
          ? RVTrim::X7
          : RVTrimII::getTailExpandUseRegNo(STI.getFeatureBits());
  if (C.back().isReturn() &&
      !C.isAvailableAcrossAndOutOfSeq(TailExpandUseReg, RegInfo)) {
    LLVM_DEBUG(dbgs() << "MBB:\n" << *C.getMBB());
    LLVM_DEBUG(dbgs() << "Cannot be outlined between: " << C.front() << "and "
                      << C.back());
    LLVM_DEBUG(dbgs() << "Because the tail-call register is live across "
                         "the proposed outlined function call\n");
    return true;
  }

  // If last instruction is return then we can rely on
  // the verification already performed in the getOutliningTypeImpl.
  if (C.back().isReturn()) {
    assert(!cannotInsertTailCall(*C.getMBB()) &&
           "The candidate who uses return instruction must be outlined "
           "using tail call");
    return false;
  }

  // Filter out candidates where the X5 register (t0) can't be used to setup
  // the function call.
  if (!C.isAvailableInsideSeq(RVTrim::X5, RegInfo))
    return true;

  // If X5 is available in the region, use X5 directly (MachineOutlinerDefault).
  if (C.isAvailableAcrossAndOutOfSeq(RVTrim::X5, RegInfo))
    return false;

  // Otherwise, try to save X5 into t1-t6 (MachineOutlinerRegSave).
  if (STI.getCLOpts().outliner_regsave && findRegisterToSaveX5To(C, RegInfo))
    return false;

  return true;
}

std::optional<std::unique_ptr<outliner::OutlinedFunction>>
RVTrimInstrInfo::getOutliningCandidateInfo(
    const MachineModuleInfo &MMI,
    std::vector<outliner::Candidate> &RepeatedSequenceLocs,
    unsigned MinRepeats) const {

  // Analyze each candidate and erase the ones that are not viable.
  llvm::erase_if(RepeatedSequenceLocs, [this](auto Candidate) {
    return analyzeCandidate(Candidate);
  });

  // If the sequence doesn't have enough candidates left, then we're done.
  if (RepeatedSequenceLocs.size() < MinRepeats)
    return std::nullopt;

  // Each RepeatedSequenceLoc is identical.
  outliner::Candidate &Candidate = RepeatedSequenceLocs[0];
  unsigned InstrSizeCExt =
      Candidate.getMF()->getSubtarget<RVTrimSubtarget>().hasStdExtZca() ? 2 : 4;
  unsigned CallOverhead = 0, FrameOverhead = 0;

  // Count the number of CFI instructions in the candidate, if present.
  unsigned CFICount = 0;
  for (auto &I : Candidate) {
    if (I.isCFIInstruction())
      CFICount++;
  }

  // Ensure CFI coverage matches: comparing the number of CFIs in the candidate
  // with the total number of CFIs in the parent function for each candidate.
  // Outlining only a subset of a function’s CFIs would split the unwind state
  // across two code regions and lead to incorrect address offsets between the
  // outlined body and the remaining code. To preserve correct unwind info, we
  // only outline when all CFIs in the function can be outlined together.
  for (outliner::Candidate &C : RepeatedSequenceLocs) {
    std::vector<MCCFIInstruction> CFIInstructions =
        C.getMF()->getFrameInstructions();

    if (CFICount > 0 && CFICount != CFIInstructions.size())
      return std::nullopt;
  }

  MachineOutlinerConstructionID MOCI = MachineOutlinerDefault;
  if (Candidate.back().isReturn()) {
    MOCI = MachineOutlinerTailCall;
    // tail call = auipc + jalr in the worst case without linker relaxation.
    // FIXME: This code suggests the JALR can be compressed - how?
    CallOverhead = 4 + InstrSizeCExt;
    // Using tail call we move ret instruction from caller to callee.
    FrameOverhead = 0;
  } else {
    // call t0, function = 8 bytes.
    CallOverhead = 8;
    // jr t0 = 4 bytes, 2 bytes if compressed instructions are enabled.
    FrameOverhead = InstrSizeCExt;
  }

  // If we have CFI instructions, we can only outline if the outlined section
  // can be a tail call.
  if (MOCI != MachineOutlinerTailCall && CFICount > 0)
    return std::nullopt;

  if (STI.getCLOpts().outliner_regsave && MOCI == MachineOutlinerDefault) {
    // Set per-candidate overhead based on X5 availability
    for (auto &C : RepeatedSequenceLocs) {

      if (C.isAvailableAcrossAndOutOfSeq(RVTrim::X5, RegInfo)) {
        // X5 is available, just need the call
        unsigned CandCallOverhead = 8;
        C.setCallInfo(MachineOutlinerDefault, CandCallOverhead);
      } else {
        // X5 unavailable, need save + call + restore
        // Save (2-4) + Call (8) + Restore (2-4)
        unsigned CandCallOverhead = InstrSizeCExt + 8 + InstrSizeCExt;
        C.setCallInfo(MachineOutlinerRegSave, CandCallOverhead);
      }
    }
  } else {
    for (auto &C : RepeatedSequenceLocs)
      C.setCallInfo(MOCI, CallOverhead);
  }

  unsigned SequenceSize = 0;
  for (auto &MI : Candidate)
    SequenceSize += getInstSizeInBytes(MI);

  return std::make_unique<outliner::OutlinedFunction>(
      RepeatedSequenceLocs, SequenceSize, FrameOverhead, MOCI);
}

outliner::InstrType
RVTrimInstrInfo::getOutliningTypeImpl(const MachineModuleInfo &MMI,
                                     MachineBasicBlock::iterator &MBBI,
                                     unsigned Flags) const {
  MachineInstr &MI = *MBBI;
  MachineBasicBlock *MBB = MI.getParent();
  const TargetRegisterInfo *TRI =
      MBB->getParent()->getSubtarget().getRegisterInfo();
  const auto &F = MI.getMF()->getFunction();

  // We can only outline CFI instructions if we will tail call the outlined
  // function, or fix up the CFI offsets. Currently, CFI instructions are
  // outlined only if in a tail call.
  if (MI.isCFIInstruction())
    return outliner::InstrType::Legal;

  if (cannotInsertTailCall(*MBB) &&
      (MI.isReturn() || isMIModifiesReg(MI, TRI, RVTrim::X5)))
    return outliner::InstrType::Illegal;

  // Make sure the operands don't reference something unsafe.
  for (const auto &MO : MI.operands()) {

    // pcrel-hi and pcrel-lo can't put in separate sections, filter that out
    // if any possible.
    if (MO.getTargetFlags() == RVTrimII::MO_PCREL_LO &&
        (MI.getMF()->getTarget().getFunctionSections() || F.hasComdat() ||
         F.hasSection() || F.getSectionPrefix()))
      return outliner::InstrType::Illegal;
  }

  if (isLPAD(MI))
    return outliner::InstrType::Illegal;

  return outliner::InstrType::Legal;
}

void RVTrimInstrInfo::buildOutlinedFrame(
    MachineBasicBlock &MBB, MachineFunction &MF,
    const outliner::OutlinedFunction &OF) const {

  if (OF.FrameConstructionID == MachineOutlinerTailCall)
    return;

  MBB.addLiveIn(RVTrim::X5);

  // Add in a return instruction to the end of the outlined frame.
  MBB.insert(MBB.end(), BuildMI(MF, DebugLoc(), get(RVTrim::JALR), RVTrim::X0)
                            .addReg(RVTrim::X5)
                            .addImm(0));
}

MachineBasicBlock::iterator RVTrimInstrInfo::insertOutlinedCall(
    Module &M, MachineBasicBlock &MBB, MachineBasicBlock::iterator &It,
    MachineFunction &MF, outliner::Candidate &C) const {

  if (C.CallConstructionID == MachineOutlinerTailCall) {
    const RVTrimMachineFunctionInfo *RVFI =
        MF.getInfo<RVTrimMachineFunctionInfo>();
    unsigned TailOpc =
        RVFI->hasCFProtectionBranch() ? RVTrim::PseudoTAILX7 : RVTrim::PseudoTAIL;
    It = MBB.insert(It, BuildMI(MF, DebugLoc(), get(TailOpc))
                            .addGlobalAddress(M.getNamedValue(MF.getName()),
                                              /*Offset=*/0, RVTrimII::MO_CALL));
    return It;
  }

  if (C.CallConstructionID == MachineOutlinerRegSave) {
    Register SaveReg = findRegisterToSaveX5To(C, RegInfo);
    assert(SaveReg && "Cannot find an available register to save/restore X5.");

    // Save: ADDI SaveReg, X5, 0 (equivalent to MV SaveReg, X5)
    It = MBB.insert(It, BuildMI(MF, DebugLoc(), get(RVTrim::ADDI), SaveReg)
                            .addReg(RVTrim::X5)
                            .addImm(0));
    It++;

    // Call: PseudoCALLReg X5
    It = MBB.insert(
        It, BuildMI(MF, DebugLoc(), get(RVTrim::PseudoCALLReg), RVTrim::X5)
                .addGlobalAddress(M.getNamedValue(MF.getName()), 0,
                                  RVTrimII::MO_CALL));
    MachineBasicBlock::iterator CallPt = It;
    It++;

    // Restore: ADDI X5, SaveReg, 0 (equivalent to MV X5, SaveReg)
    It = MBB.insert(It, BuildMI(MF, DebugLoc(), get(RVTrim::ADDI), RVTrim::X5)
                            .addReg(SaveReg)
                            .addImm(0));

    return CallPt;
  }

  // Add in a call instruction to the outlined function at the given location.
  It = MBB.insert(It,
                  BuildMI(MF, DebugLoc(), get(RVTrim::PseudoCALLReg), RVTrim::X5)
                      .addGlobalAddress(M.getNamedValue(MF.getName()), 0,
                                        RVTrimII::MO_CALL));
  return It;
}

void RVTrimInstrInfo::buildClearRegister(Register Reg, MachineBasicBlock &MBB,
                                        MachineBasicBlock::iterator Iter,
                                        DebugLoc &DL,
                                        bool AllowSideEffects) const {

  const MachineFunction &MF = *MBB.getParent();
  const RVTrimRegisterInfo &TRI = *STI.getRegisterInfo();

  if (TRI.isGeneralPurposeRegister(MF, Reg)) {
    BuildMI(MBB, Iter, DL, get(RVTrim::PseudoClearGPR), Reg);
  } else if (RVTrim::FPR32RegClass.contains(Reg)) {
    BuildMI(MBB, Iter, DL, get(RVTrim::PseudoClearFPR32), Reg);
  } else if (RVTrim::FPR64RegClass.contains(Reg)) {
    BuildMI(MBB, Iter, DL, get(RVTrim::PseudoClearFPR64), Reg);
  } else if (RVTrim::FPR128RegClass.contains(Reg)) {
    BuildMI(MBB, Iter, DL, get(RVTrim::PseudoClearFPR128), Reg);
  } else if (RVTrim::VRRegClass.contains(Reg)) {
    BuildMI(MBB, Iter, DL, get(RVTrim::PseudoClearVR), Reg);
  } else {
    llvm::reportFatalInternalError(
        "buildClearRegister is not implemented for " + TRI.getRegAsmName(Reg));
  }
}

std::optional<RegImmPair> RVTrimInstrInfo::isAddImmediate(const MachineInstr &MI,
                                                         Register Reg) const {
  // TODO: Handle cases where Reg is a super- or sub-register of the
  // destination register.
  const MachineOperand &Op0 = MI.getOperand(0);
  if (!Op0.isReg() || Reg != Op0.getReg())
    return std::nullopt;

  // Don't consider ADDIW as a candidate because the caller may not be aware
  // of its sign extension behaviour.
  if (MI.getOpcode() == RVTrim::ADDI && MI.getOperand(1).isReg() &&
      MI.getOperand(2).isImm())
    return RegImmPair{MI.getOperand(1).getReg(), MI.getOperand(2).getImm()};

  return std::nullopt;
}

// MIR printer helper function to annotate Operands with a comment.
std::string RVTrimInstrInfo::createMIROperandComment(const MachineInstr &MI,
                                                    const MachineOperand &Op,
                                                    unsigned OpIdx) const {
  // Print a generic comment for this operand if there is one.
  std::string GenericComment =
      TargetInstrInfo::createMIROperandComment(MI, Op, OpIdx);
  if (!GenericComment.empty())
    return GenericComment;

  const MCInstrDesc &Desc = MI.getDesc();
  if (OpIdx >= Desc.getNumOperands())
    return std::string();

  std::string Comment;
  raw_string_ostream OS(Comment);

  const MCOperandInfo &OpInfo = Desc.operands()[OpIdx];

  // Print the full VType operand of vsetvli/vsetivli instructions, and the SEW
  // operand of vector codegen pseudos.
  switch (OpInfo.OperandType) {
  case RVTrimOp::OPERAND_VTYPEI10:
  case RVTrimOp::OPERAND_VTYPEI11: {
    unsigned Imm = Op.getImm();
    RISCVVType::printVType(Imm, OS);
    break;
  }
  case RVTrimOp::OPERAND_XSFMM_VTYPE: {
    unsigned Imm = Op.getImm();
    RISCVVType::printXSfmmVType(Imm, OS);
    break;
  }
  case RVTrimOp::OPERAND_XSFMM_TWIDEN: {
    unsigned Imm = Op.getImm();
    OS << "w" << Imm;
    break;
  }
  case RVTrimOp::OPERAND_SEW:
  case RVTrimOp::OPERAND_SEW_MASK: {
    unsigned Log2SEW = Op.getImm();
    unsigned SEW = Log2SEW ? 1 << Log2SEW : 8;
    assert(RISCVVType::isValidSEW(SEW) && "Unexpected SEW");
    OS << "e" << SEW;
    break;
  }
  case RVTrimOp::OPERAND_VEC_POLICY: {
    unsigned Policy = Op.getImm();
    assert(Policy <= (RISCVVType::TAIL_AGNOSTIC | RISCVVType::MASK_AGNOSTIC) &&
           "Invalid Policy Value");
    OS << (Policy & RISCVVType::TAIL_AGNOSTIC ? "ta" : "tu") << ", "
       << (Policy & RISCVVType::MASK_AGNOSTIC ? "ma" : "mu");
    break;
  }
  case RVTrimOp::OPERAND_AVL:
    if (Op.isImm() && Op.getImm() == -1)
      OS << "vl=VLMAX";
    else
      OS << "vl";
    break;
  case RVTrimOp::OPERAND_VEC_RM:
    if (RVTrimII::usesVXRM(Desc.TSFlags)) {
      assert(RVTrimVXRndMode::isValidRoundingMode(Op.getImm()));
      auto VXRM = static_cast<RVTrimVXRndMode::RoundingMode>(Op.getImm());
      OS << "vxrm=" << RVTrimVXRndMode::roundingModeToString(VXRM);
    } else {
      assert(RVTrimFPRndMode::isValidRoundingMode(Op.getImm()));
      auto FRM = static_cast<RVTrimFPRndMode::RoundingMode>(Op.getImm());
      OS << "frm=" << RVTrimFPRndMode::roundingModeToString(FRM);
    }
    break;
  }

  return Comment;
}

// clang-format off
#define CASE_RVV_OPCODE_UNMASK_LMUL(OP, LMUL)                                 \
  RVTrim::Pseudo##OP##_##LMUL

#define CASE_RVV_OPCODE_MASK_LMUL(OP, LMUL)                                   \
  RVTrim::Pseudo##OP##_##LMUL##_MASK

#define CASE_RVV_OPCODE_LMUL(OP, LMUL)                                        \
  CASE_RVV_OPCODE_UNMASK_LMUL(OP, LMUL):                                      \
  case CASE_RVV_OPCODE_MASK_LMUL(OP, LMUL)

#define CASE_RVV_OPCODE_UNMASK_WIDEN(OP)                                      \
  CASE_RVV_OPCODE_UNMASK_LMUL(OP, MF8):                                       \
  case CASE_RVV_OPCODE_UNMASK_LMUL(OP, MF4):                                  \
  case CASE_RVV_OPCODE_UNMASK_LMUL(OP, MF2):                                  \
  case CASE_RVV_OPCODE_UNMASK_LMUL(OP, M1):                                   \
  case CASE_RVV_OPCODE_UNMASK_LMUL(OP, M2):                                   \
  case CASE_RVV_OPCODE_UNMASK_LMUL(OP, M4)

#define CASE_RVV_OPCODE_UNMASK(OP)                                            \
  CASE_RVV_OPCODE_UNMASK_WIDEN(OP):                                           \
  case CASE_RVV_OPCODE_UNMASK_LMUL(OP, M8)

#define CASE_RVV_OPCODE_MASK_WIDEN(OP)                                        \
  CASE_RVV_OPCODE_MASK_LMUL(OP, MF8):                                         \
  case CASE_RVV_OPCODE_MASK_LMUL(OP, MF4):                                    \
  case CASE_RVV_OPCODE_MASK_LMUL(OP, MF2):                                    \
  case CASE_RVV_OPCODE_MASK_LMUL(OP, M1):                                     \
  case CASE_RVV_OPCODE_MASK_LMUL(OP, M2):                                     \
  case CASE_RVV_OPCODE_MASK_LMUL(OP, M4)

#define CASE_RVV_OPCODE_MASK(OP)                                              \
  CASE_RVV_OPCODE_MASK_WIDEN(OP):                                             \
  case CASE_RVV_OPCODE_MASK_LMUL(OP, M8)

#define CASE_RVV_OPCODE_WIDEN(OP)                                             \
  CASE_RVV_OPCODE_UNMASK_WIDEN(OP):                                           \
  case CASE_RVV_OPCODE_MASK_WIDEN(OP)

#define CASE_RVV_OPCODE(OP)                                                   \
  CASE_RVV_OPCODE_UNMASK(OP):                                                 \
  case CASE_RVV_OPCODE_MASK(OP)
// clang-format on

// clang-format off
#define CASE_VMA_OPCODE_COMMON(OP, TYPE, LMUL)                                 \
  RVTrim::PseudoV##OP##_##TYPE##_##LMUL

#define CASE_VMA_OPCODE_LMULS(OP, TYPE)                                        \
  CASE_VMA_OPCODE_COMMON(OP, TYPE, MF8):                                       \
  case CASE_VMA_OPCODE_COMMON(OP, TYPE, MF4):                                  \
  case CASE_VMA_OPCODE_COMMON(OP, TYPE, MF2):                                  \
  case CASE_VMA_OPCODE_COMMON(OP, TYPE, M1):                                   \
  case CASE_VMA_OPCODE_COMMON(OP, TYPE, M2):                                   \
  case CASE_VMA_OPCODE_COMMON(OP, TYPE, M4):                                   \
  case CASE_VMA_OPCODE_COMMON(OP, TYPE, M8)

// VFMA instructions are SEW specific.
#define CASE_VFMA_OPCODE_COMMON(OP, TYPE, LMUL, SEW)                           \
  RVTrim::PseudoV##OP##_##TYPE##_##LMUL##_##SEW

#define CASE_VFMA_OPCODE_LMULS_M1(OP, TYPE, SEW)                               \
  CASE_VFMA_OPCODE_COMMON(OP, TYPE, M1, SEW):                                  \
  case CASE_VFMA_OPCODE_COMMON(OP, TYPE, M2, SEW):                             \
  case CASE_VFMA_OPCODE_COMMON(OP, TYPE, M4, SEW):                             \
  case CASE_VFMA_OPCODE_COMMON(OP, TYPE, M8, SEW)

#define CASE_VFMA_OPCODE_LMULS_MF2(OP, TYPE, SEW)                              \
  CASE_VFMA_OPCODE_COMMON(OP, TYPE, MF2, SEW):                                 \
  case CASE_VFMA_OPCODE_LMULS_M1(OP, TYPE, SEW)

#define CASE_VFMA_OPCODE_LMULS_MF4(OP, TYPE, SEW)                              \
  CASE_VFMA_OPCODE_COMMON(OP, TYPE, MF4, SEW):                                 \
  case CASE_VFMA_OPCODE_LMULS_MF2(OP, TYPE, SEW)

#define CASE_VFMA_OPCODE_VV(OP)                                                \
  CASE_VFMA_OPCODE_LMULS_MF4(OP, VV, E16):                                     \
  case CASE_VFMA_OPCODE_LMULS_MF4(OP##_ALT, VV, E16):                          \
  case CASE_VFMA_OPCODE_LMULS_MF2(OP, VV, E32):                                \
  case CASE_VFMA_OPCODE_LMULS_M1(OP, VV, E64)

#define CASE_VFMA_SPLATS(OP)                                                   \
  CASE_VFMA_OPCODE_LMULS_MF4(OP, VFPR16, E16):                                 \
  case CASE_VFMA_OPCODE_LMULS_MF4(OP##_ALT, VFPR16, E16):                      \
  case CASE_VFMA_OPCODE_LMULS_MF2(OP, VFPR32, E32):                            \
  case CASE_VFMA_OPCODE_LMULS_M1(OP, VFPR64, E64)
// clang-format on

bool RVTrimInstrInfo::findCommutedOpIndices(const MachineInstr &MI,
                                           unsigned &SrcOpIdx1,
                                           unsigned &SrcOpIdx2) const {
  const MCInstrDesc &Desc = MI.getDesc();
  if (!Desc.isCommutable())
    return false;

  switch (MI.getOpcode()) {
  case RVTrim::TH_MVEQZ:
  case RVTrim::TH_MVNEZ:
    // We can't commute operands if operand 2 (i.e., rs1 in
    // mveqz/mvnez rd,rs1,rs2) is the zero-register (as it is
    // not valid as the in/out-operand 1).
    if (MI.getOperand(2).getReg() == RVTrim::X0)
      return false;
    // Operands 1 and 2 are commutable, if we switch the opcode.
    return fixCommutedOpIndices(SrcOpIdx1, SrcOpIdx2, 1, 2);
  case RVTrim::QC_SELECTIEQ:
  case RVTrim::QC_SELECTINE:
  case RVTrim::QC_SELECTIIEQ:
  case RVTrim::QC_SELECTIINE:
    return fixCommutedOpIndices(SrcOpIdx1, SrcOpIdx2, 1, 2);
  case RVTrim::QC_MVEQ:
  case RVTrim::QC_MVNE:
  case RVTrim::QC_MVLT:
  case RVTrim::QC_MVGE:
  case RVTrim::QC_MVLTU:
  case RVTrim::QC_MVGEU:
  case RVTrim::QC_MVEQI:
  case RVTrim::QC_MVNEI:
  case RVTrim::QC_MVLTI:
  case RVTrim::QC_MVGEI:
  case RVTrim::QC_MVLTUI:
  case RVTrim::QC_MVGEUI:
    return fixCommutedOpIndices(SrcOpIdx1, SrcOpIdx2, 1, 4);
  case RVTrim::TH_MULA:
  case RVTrim::TH_MULAW:
  case RVTrim::TH_MULAH:
  case RVTrim::TH_MULS:
  case RVTrim::TH_MULSW:
  case RVTrim::TH_MULSH:
    // Operands 2 and 3 are commutable.
    return fixCommutedOpIndices(SrcOpIdx1, SrcOpIdx2, 2, 3);
  case RVTrim::PseudoCCMOVGPRNoX0:
  case RVTrim::PseudoCCMOVGPR:
    // Operands 1 and 2 are commutable.
    return fixCommutedOpIndices(SrcOpIdx1, SrcOpIdx2, 1, 2);
  case CASE_RVV_OPCODE(VADD_VV):
  case CASE_RVV_OPCODE(VAND_VV):
  case CASE_RVV_OPCODE(VOR_VV):
  case CASE_RVV_OPCODE(VXOR_VV):
  case CASE_RVV_OPCODE_MASK(VMSEQ_VV):
  case CASE_RVV_OPCODE_MASK(VMSNE_VV):
  case CASE_RVV_OPCODE(VMIN_VV):
  case CASE_RVV_OPCODE(VMINU_VV):
  case CASE_RVV_OPCODE(VMAX_VV):
  case CASE_RVV_OPCODE(VMAXU_VV):
  case CASE_RVV_OPCODE(VMUL_VV):
  case CASE_RVV_OPCODE(VMULH_VV):
  case CASE_RVV_OPCODE(VMULHU_VV):
  case CASE_RVV_OPCODE_WIDEN(VWADD_VV):
  case CASE_RVV_OPCODE_WIDEN(VWADDU_VV):
  case CASE_RVV_OPCODE_WIDEN(VWMUL_VV):
  case CASE_RVV_OPCODE_WIDEN(VWMULU_VV):
  case CASE_RVV_OPCODE_WIDEN(VWMACC_VV):
  case CASE_RVV_OPCODE_WIDEN(VWMACCU_VV):
  case CASE_RVV_OPCODE(VABD_VV):
  case CASE_RVV_OPCODE(VABDU_VV):
  case CASE_RVV_OPCODE_WIDEN(VWABDA_VV):
  case CASE_RVV_OPCODE_WIDEN(VWABDAU_VV):
  case CASE_RVV_OPCODE_UNMASK(VADC_VVM):
  case CASE_RVV_OPCODE(VSADD_VV):
  case CASE_RVV_OPCODE(VSADDU_VV):
  case CASE_RVV_OPCODE(VAADD_VV):
  case CASE_RVV_OPCODE(VAADDU_VV):
  case CASE_RVV_OPCODE(VSMUL_VV):
  case CASE_RVV_OPCODE_LMUL(VDOT4A_VV, MF2):
  case CASE_RVV_OPCODE_LMUL(VDOT4A_VV, M1):
  case CASE_RVV_OPCODE_LMUL(VDOT4A_VV, M2):
  case CASE_RVV_OPCODE_LMUL(VDOT4A_VV, M4):
  case CASE_RVV_OPCODE_LMUL(VDOT4A_VV, M8):
  case CASE_RVV_OPCODE_LMUL(VDOT4AU_VV, MF2):
  case CASE_RVV_OPCODE_LMUL(VDOT4AU_VV, M1):
  case CASE_RVV_OPCODE_LMUL(VDOT4AU_VV, M2):
  case CASE_RVV_OPCODE_LMUL(VDOT4AU_VV, M4):
  case CASE_RVV_OPCODE_LMUL(VDOT4AU_VV, M8):
    // Operands 2 and 3 are commutable.
    return fixCommutedOpIndices(SrcOpIdx1, SrcOpIdx2, 2, 3);
  case CASE_VFMA_SPLATS(FMADD):
  case CASE_VFMA_SPLATS(FMSUB):
  case CASE_VFMA_SPLATS(FMACC):
  case CASE_VFMA_SPLATS(FMSAC):
  case CASE_VFMA_SPLATS(FNMADD):
  case CASE_VFMA_SPLATS(FNMSUB):
  case CASE_VFMA_SPLATS(FNMACC):
  case CASE_VFMA_SPLATS(FNMSAC):
  case CASE_VFMA_OPCODE_VV(FMACC):
  case CASE_VFMA_OPCODE_VV(FMSAC):
  case CASE_VFMA_OPCODE_VV(FNMACC):
  case CASE_VFMA_OPCODE_VV(FNMSAC):
  case CASE_VMA_OPCODE_LMULS(MADD, VX):
  case CASE_VMA_OPCODE_LMULS(NMSUB, VX):
  case CASE_VMA_OPCODE_LMULS(MACC, VX):
  case CASE_VMA_OPCODE_LMULS(NMSAC, VX):
  case CASE_VMA_OPCODE_LMULS(MACC, VV):
  case CASE_VMA_OPCODE_LMULS(NMSAC, VV): {
    // If the tail policy is undisturbed we can't commute.
    assert(RVTrimII::hasVecPolicyOp(MI.getDesc().TSFlags));
    if ((MI.getOperand(RVTrimII::getVecPolicyOpNum(MI.getDesc())).getImm() &
         1) == 0)
      return false;

    // For these instructions we can only swap operand 1 and operand 3 by
    // changing the opcode.
    unsigned CommutableOpIdx1 = 1;
    unsigned CommutableOpIdx2 = 3;
    if (!fixCommutedOpIndices(SrcOpIdx1, SrcOpIdx2, CommutableOpIdx1,
                              CommutableOpIdx2))
      return false;
    return true;
  }
  case CASE_VFMA_OPCODE_VV(FMADD):
  case CASE_VFMA_OPCODE_VV(FMSUB):
  case CASE_VFMA_OPCODE_VV(FNMADD):
  case CASE_VFMA_OPCODE_VV(FNMSUB):
  case CASE_VMA_OPCODE_LMULS(MADD, VV):
  case CASE_VMA_OPCODE_LMULS(NMSUB, VV): {
    // If the tail policy is undisturbed we can't commute.
    assert(RVTrimII::hasVecPolicyOp(MI.getDesc().TSFlags));
    if ((MI.getOperand(RVTrimII::getVecPolicyOpNum(MI.getDesc())).getImm() &
         1) == 0)
      return false;

    // For these instructions we have more freedom. We can commute with the
    // other multiplicand or with the addend/subtrahend/minuend.

    // Any fixed operand must be from source 1, 2 or 3.
    if (SrcOpIdx1 != CommuteAnyOperandIndex && SrcOpIdx1 > 3)
      return false;
    if (SrcOpIdx2 != CommuteAnyOperandIndex && SrcOpIdx2 > 3)
      return false;

    // It both ops are fixed one must be the tied source.
    if (SrcOpIdx1 != CommuteAnyOperandIndex &&
        SrcOpIdx2 != CommuteAnyOperandIndex && SrcOpIdx1 != 1 && SrcOpIdx2 != 1)
      return false;

    // Look for two different register operands assumed to be commutable
    // regardless of the FMA opcode. The FMA opcode is adjusted later if
    // needed.
    if (SrcOpIdx1 == CommuteAnyOperandIndex ||
        SrcOpIdx2 == CommuteAnyOperandIndex) {
      // At least one of operands to be commuted is not specified and
      // this method is free to choose appropriate commutable operands.
      unsigned CommutableOpIdx1 = SrcOpIdx1;
      if (SrcOpIdx1 == SrcOpIdx2) {
        // Both of operands are not fixed. Set one of commutable
        // operands to the tied source.
        CommutableOpIdx1 = 1;
      } else if (SrcOpIdx1 == CommuteAnyOperandIndex) {
        // Only one of the operands is not fixed.
        CommutableOpIdx1 = SrcOpIdx2;
      }

      // CommutableOpIdx1 is well defined now. Let's choose another commutable
      // operand and assign its index to CommutableOpIdx2.
      unsigned CommutableOpIdx2;
      if (CommutableOpIdx1 != 1) {
        // If we haven't already used the tied source, we must use it now.
        CommutableOpIdx2 = 1;
      } else {
        Register Op1Reg = MI.getOperand(CommutableOpIdx1).getReg();

        // The commuted operands should have different registers.
        // Otherwise, the commute transformation does not change anything and
        // is useless. We use this as a hint to make our decision.
        if (Op1Reg != MI.getOperand(2).getReg())
          CommutableOpIdx2 = 2;
        else
          CommutableOpIdx2 = 3;
      }

      // Assign the found pair of commutable indices to SrcOpIdx1 and
      // SrcOpIdx2 to return those values.
      if (!fixCommutedOpIndices(SrcOpIdx1, SrcOpIdx2, CommutableOpIdx1,
                                CommutableOpIdx2))
        return false;
    }

    return true;
  }
  }

  return TargetInstrInfo::findCommutedOpIndices(MI, SrcOpIdx1, SrcOpIdx2);
}

// clang-format off
#define CASE_VMA_CHANGE_OPCODE_COMMON(OLDOP, NEWOP, TYPE, LMUL)                \
  case RVTrim::PseudoV##OLDOP##_##TYPE##_##LMUL:                                \
    Opc = RVTrim::PseudoV##NEWOP##_##TYPE##_##LMUL;                             \
    break;

#define CASE_VMA_CHANGE_OPCODE_LMULS(OLDOP, NEWOP, TYPE)                       \
  CASE_VMA_CHANGE_OPCODE_COMMON(OLDOP, NEWOP, TYPE, MF8)                       \
  CASE_VMA_CHANGE_OPCODE_COMMON(OLDOP, NEWOP, TYPE, MF4)                       \
  CASE_VMA_CHANGE_OPCODE_COMMON(OLDOP, NEWOP, TYPE, MF2)                       \
  CASE_VMA_CHANGE_OPCODE_COMMON(OLDOP, NEWOP, TYPE, M1)                        \
  CASE_VMA_CHANGE_OPCODE_COMMON(OLDOP, NEWOP, TYPE, M2)                        \
  CASE_VMA_CHANGE_OPCODE_COMMON(OLDOP, NEWOP, TYPE, M4)                        \
  CASE_VMA_CHANGE_OPCODE_COMMON(OLDOP, NEWOP, TYPE, M8)

// VFMA depends on SEW.
#define CASE_VFMA_CHANGE_OPCODE_COMMON(OLDOP, NEWOP, TYPE, LMUL, SEW)          \
  case RVTrim::PseudoV##OLDOP##_##TYPE##_##LMUL##_##SEW:                        \
    Opc = RVTrim::PseudoV##NEWOP##_##TYPE##_##LMUL##_##SEW;                     \
    break;

#define CASE_VFMA_CHANGE_OPCODE_LMULS_M1(OLDOP, NEWOP, TYPE, SEW)              \
  CASE_VFMA_CHANGE_OPCODE_COMMON(OLDOP, NEWOP, TYPE, M1, SEW)                  \
  CASE_VFMA_CHANGE_OPCODE_COMMON(OLDOP, NEWOP, TYPE, M2, SEW)                  \
  CASE_VFMA_CHANGE_OPCODE_COMMON(OLDOP, NEWOP, TYPE, M4, SEW)                  \
  CASE_VFMA_CHANGE_OPCODE_COMMON(OLDOP, NEWOP, TYPE, M8, SEW)

#define CASE_VFMA_CHANGE_OPCODE_LMULS_MF2(OLDOP, NEWOP, TYPE, SEW)             \
  CASE_VFMA_CHANGE_OPCODE_COMMON(OLDOP, NEWOP, TYPE, MF2, SEW)                 \
  CASE_VFMA_CHANGE_OPCODE_LMULS_M1(OLDOP, NEWOP, TYPE, SEW)

#define CASE_VFMA_CHANGE_OPCODE_LMULS_MF4(OLDOP, NEWOP, TYPE, SEW)             \
  CASE_VFMA_CHANGE_OPCODE_COMMON(OLDOP, NEWOP, TYPE, MF4, SEW)                 \
  CASE_VFMA_CHANGE_OPCODE_LMULS_MF2(OLDOP, NEWOP, TYPE, SEW)

#define CASE_VFMA_CHANGE_OPCODE_VV(OLDOP, NEWOP)                               \
  CASE_VFMA_CHANGE_OPCODE_LMULS_MF4(OLDOP, NEWOP, VV, E16)                     \
  CASE_VFMA_CHANGE_OPCODE_LMULS_MF4(OLDOP##_ALT, NEWOP##_ALT, VV, E16)         \
  CASE_VFMA_CHANGE_OPCODE_LMULS_MF2(OLDOP, NEWOP, VV, E32)                     \
  CASE_VFMA_CHANGE_OPCODE_LMULS_M1(OLDOP, NEWOP, VV, E64)

#define CASE_VFMA_CHANGE_OPCODE_SPLATS(OLDOP, NEWOP)                           \
  CASE_VFMA_CHANGE_OPCODE_LMULS_MF4(OLDOP, NEWOP, VFPR16, E16)                 \
  CASE_VFMA_CHANGE_OPCODE_LMULS_MF4(OLDOP##_ALT, NEWOP##_ALT, VFPR16, E16)     \
  CASE_VFMA_CHANGE_OPCODE_LMULS_MF2(OLDOP, NEWOP, VFPR32, E32)                 \
  CASE_VFMA_CHANGE_OPCODE_LMULS_M1(OLDOP, NEWOP, VFPR64, E64)
// clang-format on

MachineInstr *RVTrimInstrInfo::commuteInstructionImpl(MachineInstr &MI,
                                                     bool NewMI,
                                                     unsigned OpIdx1,
                                                     unsigned OpIdx2) const {
  auto cloneIfNew = [NewMI](MachineInstr &MI) -> MachineInstr & {
    if (NewMI)
      return *MI.getParent()->getParent()->CloneMachineInstr(&MI);
    return MI;
  };

  switch (MI.getOpcode()) {
  case RVTrim::TH_MVEQZ:
  case RVTrim::TH_MVNEZ: {
    auto &WorkingMI = cloneIfNew(MI);
    WorkingMI.setDesc(get(MI.getOpcode() == RVTrim::TH_MVEQZ ? RVTrim::TH_MVNEZ
                                                            : RVTrim::TH_MVEQZ));
    return TargetInstrInfo::commuteInstructionImpl(WorkingMI, false, OpIdx1,
                                                   OpIdx2);
  }
  case RVTrim::QC_SELECTIEQ:
  case RVTrim::QC_SELECTINE:
  case RVTrim::QC_SELECTIIEQ:
  case RVTrim::QC_SELECTIINE:
    return TargetInstrInfo::commuteInstructionImpl(MI, NewMI, OpIdx1, OpIdx2);
  case RVTrim::QC_MVEQ:
  case RVTrim::QC_MVNE:
  case RVTrim::QC_MVLT:
  case RVTrim::QC_MVGE:
  case RVTrim::QC_MVLTU:
  case RVTrim::QC_MVGEU:
  case RVTrim::QC_MVEQI:
  case RVTrim::QC_MVNEI:
  case RVTrim::QC_MVLTI:
  case RVTrim::QC_MVGEI:
  case RVTrim::QC_MVLTUI:
  case RVTrim::QC_MVGEUI: {
    auto &WorkingMI = cloneIfNew(MI);
    WorkingMI.setDesc(get(getInverseXqcicmOpcode(MI.getOpcode())));
    return TargetInstrInfo::commuteInstructionImpl(WorkingMI, false, OpIdx1,
                                                   OpIdx2);
  }
  case RVTrim::PseudoCCMOVGPRNoX0:
  case RVTrim::PseudoCCMOVGPR: {
    // CCMOV can be commuted by inverting the condition.
    unsigned BCC = MI.getOperand(MI.getNumExplicitOperands() - 3).getImm();
    BCC = RVTrimCC::getInverseBranchOpcode(BCC);
    auto &WorkingMI = cloneIfNew(MI);
    WorkingMI.getOperand(MI.getNumExplicitOperands() - 3).setImm(BCC);
    return TargetInstrInfo::commuteInstructionImpl(WorkingMI, /*NewMI*/ false,
                                                   OpIdx1, OpIdx2);
  }
  case CASE_VFMA_SPLATS(FMACC):
  case CASE_VFMA_SPLATS(FMADD):
  case CASE_VFMA_SPLATS(FMSAC):
  case CASE_VFMA_SPLATS(FMSUB):
  case CASE_VFMA_SPLATS(FNMACC):
  case CASE_VFMA_SPLATS(FNMADD):
  case CASE_VFMA_SPLATS(FNMSAC):
  case CASE_VFMA_SPLATS(FNMSUB):
  case CASE_VFMA_OPCODE_VV(FMACC):
  case CASE_VFMA_OPCODE_VV(FMSAC):
  case CASE_VFMA_OPCODE_VV(FNMACC):
  case CASE_VFMA_OPCODE_VV(FNMSAC):
  case CASE_VMA_OPCODE_LMULS(MADD, VX):
  case CASE_VMA_OPCODE_LMULS(NMSUB, VX):
  case CASE_VMA_OPCODE_LMULS(MACC, VX):
  case CASE_VMA_OPCODE_LMULS(NMSAC, VX):
  case CASE_VMA_OPCODE_LMULS(MACC, VV):
  case CASE_VMA_OPCODE_LMULS(NMSAC, VV): {
    // It only make sense to toggle these between clobbering the
    // addend/subtrahend/minuend one of the multiplicands.
    assert((OpIdx1 == 1 || OpIdx2 == 1) && "Unexpected opcode index");
    assert((OpIdx1 == 3 || OpIdx2 == 3) && "Unexpected opcode index");
    unsigned Opc;
    switch (MI.getOpcode()) {
      default:
        llvm_unreachable("Unexpected opcode");
      CASE_VFMA_CHANGE_OPCODE_SPLATS(FMACC, FMADD)
      CASE_VFMA_CHANGE_OPCODE_SPLATS(FMADD, FMACC)
      CASE_VFMA_CHANGE_OPCODE_SPLATS(FMSAC, FMSUB)
      CASE_VFMA_CHANGE_OPCODE_SPLATS(FMSUB, FMSAC)
      CASE_VFMA_CHANGE_OPCODE_SPLATS(FNMACC, FNMADD)
      CASE_VFMA_CHANGE_OPCODE_SPLATS(FNMADD, FNMACC)
      CASE_VFMA_CHANGE_OPCODE_SPLATS(FNMSAC, FNMSUB)
      CASE_VFMA_CHANGE_OPCODE_SPLATS(FNMSUB, FNMSAC)
      CASE_VFMA_CHANGE_OPCODE_VV(FMACC, FMADD)
      CASE_VFMA_CHANGE_OPCODE_VV(FMSAC, FMSUB)
      CASE_VFMA_CHANGE_OPCODE_VV(FNMACC, FNMADD)
      CASE_VFMA_CHANGE_OPCODE_VV(FNMSAC, FNMSUB)
      CASE_VMA_CHANGE_OPCODE_LMULS(MACC, MADD, VX)
      CASE_VMA_CHANGE_OPCODE_LMULS(MADD, MACC, VX)
      CASE_VMA_CHANGE_OPCODE_LMULS(NMSAC, NMSUB, VX)
      CASE_VMA_CHANGE_OPCODE_LMULS(NMSUB, NMSAC, VX)
      CASE_VMA_CHANGE_OPCODE_LMULS(MACC, MADD, VV)
      CASE_VMA_CHANGE_OPCODE_LMULS(NMSAC, NMSUB, VV)
    }

    auto &WorkingMI = cloneIfNew(MI);
    WorkingMI.setDesc(get(Opc));
    return TargetInstrInfo::commuteInstructionImpl(WorkingMI, /*NewMI=*/false,
                                                   OpIdx1, OpIdx2);
  }
  case CASE_VFMA_OPCODE_VV(FMADD):
  case CASE_VFMA_OPCODE_VV(FMSUB):
  case CASE_VFMA_OPCODE_VV(FNMADD):
  case CASE_VFMA_OPCODE_VV(FNMSUB):
  case CASE_VMA_OPCODE_LMULS(MADD, VV):
  case CASE_VMA_OPCODE_LMULS(NMSUB, VV): {
    assert((OpIdx1 == 1 || OpIdx2 == 1) && "Unexpected opcode index");
    // If one of the operands, is the addend we need to change opcode.
    // Otherwise we're just swapping 2 of the multiplicands.
    if (OpIdx1 == 3 || OpIdx2 == 3) {
      unsigned Opc;
      switch (MI.getOpcode()) {
        default:
          llvm_unreachable("Unexpected opcode");
        CASE_VFMA_CHANGE_OPCODE_VV(FMADD, FMACC)
        CASE_VFMA_CHANGE_OPCODE_VV(FMSUB, FMSAC)
        CASE_VFMA_CHANGE_OPCODE_VV(FNMADD, FNMACC)
        CASE_VFMA_CHANGE_OPCODE_VV(FNMSUB, FNMSAC)
        CASE_VMA_CHANGE_OPCODE_LMULS(MADD, MACC, VV)
        CASE_VMA_CHANGE_OPCODE_LMULS(NMSUB, NMSAC, VV)
      }

      auto &WorkingMI = cloneIfNew(MI);
      WorkingMI.setDesc(get(Opc));
      return TargetInstrInfo::commuteInstructionImpl(WorkingMI, /*NewMI=*/false,
                                                     OpIdx1, OpIdx2);
    }
    // Let the default code handle it.
    break;
  }
  }

  return TargetInstrInfo::commuteInstructionImpl(MI, NewMI, OpIdx1, OpIdx2);
}

#undef CASE_VMA_CHANGE_OPCODE_COMMON
#undef CASE_VMA_CHANGE_OPCODE_LMULS
#undef CASE_VFMA_CHANGE_OPCODE_COMMON
#undef CASE_VFMA_CHANGE_OPCODE_LMULS_M1
#undef CASE_VFMA_CHANGE_OPCODE_LMULS_MF2
#undef CASE_VFMA_CHANGE_OPCODE_LMULS_MF4
#undef CASE_VFMA_CHANGE_OPCODE_VV
#undef CASE_VFMA_CHANGE_OPCODE_SPLATS

#undef CASE_RVV_OPCODE_UNMASK_LMUL
#undef CASE_RVV_OPCODE_MASK_LMUL
#undef CASE_RVV_OPCODE_LMUL
#undef CASE_RVV_OPCODE_UNMASK_WIDEN
#undef CASE_RVV_OPCODE_UNMASK
#undef CASE_RVV_OPCODE_MASK_WIDEN
#undef CASE_RVV_OPCODE_MASK
#undef CASE_RVV_OPCODE_WIDEN
#undef CASE_RVV_OPCODE

#undef CASE_VMA_OPCODE_COMMON
#undef CASE_VMA_OPCODE_LMULS
#undef CASE_VFMA_OPCODE_COMMON
#undef CASE_VFMA_OPCODE_LMULS_M1
#undef CASE_VFMA_OPCODE_LMULS_MF2
#undef CASE_VFMA_OPCODE_LMULS_MF4
#undef CASE_VFMA_OPCODE_VV
#undef CASE_VFMA_SPLATS

bool RVTrimInstrInfo::simplifyInstruction(MachineInstr &MI) const {
  switch (MI.getOpcode()) {
  default:
    break;
  case RVTrim::ADD:
  case RVTrim::OR:
  case RVTrim::XOR:
    // Normalize (so we hit the next if clause).
    // add/[x]or rd, zero, rs => add/[x]or rd, rs, zero
    if (MI.getOperand(1).getReg() == RVTrim::X0)
      commuteInstruction(MI);
    // add/[x]or rd, rs, zero => addi rd, rs, 0
    if (MI.getOperand(2).getReg() == RVTrim::X0) {
      MI.getOperand(2).ChangeToImmediate(0);
      MI.setDesc(get(RVTrim::ADDI));
      return true;
    }
    // xor rd, rs, rs => addi rd, zero, 0
    if (MI.getOpcode() == RVTrim::XOR &&
        MI.getOperand(1).getReg() == MI.getOperand(2).getReg()) {
      MI.getOperand(1).setReg(RVTrim::X0);
      MI.getOperand(2).ChangeToImmediate(0);
      MI.setDesc(get(RVTrim::ADDI));
      return true;
    }
    break;
  case RVTrim::ORI:
  case RVTrim::XORI:
    // [x]ori rd, zero, N => addi rd, zero, N
    if (MI.getOperand(1).getReg() == RVTrim::X0) {
      MI.setDesc(get(RVTrim::ADDI));
      return true;
    }
    break;
  case RVTrim::SUB:
    // sub rd, rs, zero => addi rd, rs, 0
    if (MI.getOperand(2).getReg() == RVTrim::X0) {
      MI.getOperand(2).ChangeToImmediate(0);
      MI.setDesc(get(RVTrim::ADDI));
      return true;
    }
    break;
  case RVTrim::SUBW:
    // subw rd, rs, zero => addiw rd, rs, 0
    if (MI.getOperand(2).getReg() == RVTrim::X0) {
      MI.getOperand(2).ChangeToImmediate(0);
      MI.setDesc(get(RVTrim::ADDIW));
      return true;
    }
    break;
  case RVTrim::ADDW:
    // Normalize (so we hit the next if clause).
    // addw rd, zero, rs => addw rd, rs, zero
    if (MI.getOperand(1).getReg() == RVTrim::X0)
      commuteInstruction(MI);
    // addw rd, rs, zero => addiw rd, rs, 0
    if (MI.getOperand(2).getReg() == RVTrim::X0) {
      MI.getOperand(2).ChangeToImmediate(0);
      MI.setDesc(get(RVTrim::ADDIW));
      return true;
    }
    break;
  case RVTrim::SH1ADD:
  case RVTrim::SH1ADD_UW:
  case RVTrim::SH2ADD:
  case RVTrim::SH2ADD_UW:
  case RVTrim::SH3ADD:
  case RVTrim::SH3ADD_UW:
    // shNadd[.uw] rd, zero, rs => addi rd, rs, 0
    if (MI.getOperand(1).getReg() == RVTrim::X0) {
      MI.removeOperand(1);
      MI.addOperand(MachineOperand::CreateImm(0));
      MI.setDesc(get(RVTrim::ADDI));
      return true;
    }
    // shNadd[.uw] rd, rs, zero => slli[.uw] rd, rs, N
    if (MI.getOperand(2).getReg() == RVTrim::X0) {
      MI.removeOperand(2);
      unsigned Opc = MI.getOpcode();
      if (Opc == RVTrim::SH1ADD_UW || Opc == RVTrim::SH2ADD_UW ||
          Opc == RVTrim::SH3ADD_UW) {
        MI.addOperand(MachineOperand::CreateImm(getSHXADDUWShiftAmount(Opc)));
        MI.setDesc(get(RVTrim::SLLI_UW));
        return true;
      }
      MI.addOperand(MachineOperand::CreateImm(getSHXADDShiftAmount(Opc)));
      MI.setDesc(get(RVTrim::SLLI));
      return true;
    }
    break;
  case RVTrim::AND:
  case RVTrim::MUL:
  case RVTrim::MULH:
  case RVTrim::MULHSU:
  case RVTrim::MULHU:
  case RVTrim::MULW:
    // and rd, zero, rs => addi rd, zero, 0
    // mul* rd, zero, rs => addi rd, zero, 0
    // and rd, rs, zero => addi rd, zero, 0
    // mul* rd, rs, zero => addi rd, zero, 0
    if (MI.getOperand(1).getReg() == RVTrim::X0 ||
        MI.getOperand(2).getReg() == RVTrim::X0) {
      MI.getOperand(1).setReg(RVTrim::X0);
      MI.getOperand(2).ChangeToImmediate(0);
      MI.setDesc(get(RVTrim::ADDI));
      return true;
    }
    break;
  case RVTrim::ANDI:
    // andi rd, zero, C => addi rd, zero, 0
    if (MI.getOperand(1).getReg() == RVTrim::X0) {
      MI.getOperand(2).setImm(0);
      MI.setDesc(get(RVTrim::ADDI));
      return true;
    }
    break;
  case RVTrim::SLL:
  case RVTrim::SRL:
  case RVTrim::SRA:
    // shift rd, zero, rs => addi rd, zero, 0
    if (MI.getOperand(1).getReg() == RVTrim::X0) {
      MI.getOperand(2).ChangeToImmediate(0);
      MI.setDesc(get(RVTrim::ADDI));
      return true;
    }
    // shift rd, rs, zero => addi rd, rs, 0
    if (MI.getOperand(2).getReg() == RVTrim::X0) {
      MI.getOperand(2).ChangeToImmediate(0);
      MI.setDesc(get(RVTrim::ADDI));
      return true;
    }
    break;
  case RVTrim::SLLW:
  case RVTrim::SRLW:
  case RVTrim::SRAW:
    // shiftw rd, zero, rs => addi rd, zero, 0
    if (MI.getOperand(1).getReg() == RVTrim::X0) {
      MI.getOperand(2).ChangeToImmediate(0);
      MI.setDesc(get(RVTrim::ADDI));
      return true;
    }
    break;
  case RVTrim::SLLI:
  case RVTrim::SRLI:
  case RVTrim::SRAI:
  case RVTrim::SLLIW:
  case RVTrim::SRLIW:
  case RVTrim::SRAIW:
  case RVTrim::SLLI_UW:
    // shiftimm rd, zero, N => addi rd, zero, 0
    if (MI.getOperand(1).getReg() == RVTrim::X0) {
      MI.getOperand(2).setImm(0);
      MI.setDesc(get(RVTrim::ADDI));
      return true;
    }
    break;
  case RVTrim::SLTU:
  case RVTrim::ADD_UW:
    // sltu rd, zero, zero => addi rd, zero, 0
    // add.uw rd, zero, zero => addi rd, zero, 0
    if (MI.getOperand(1).getReg() == RVTrim::X0 &&
        MI.getOperand(2).getReg() == RVTrim::X0) {
      MI.getOperand(2).ChangeToImmediate(0);
      MI.setDesc(get(RVTrim::ADDI));
      return true;
    }
    // add.uw rd, zero, rs => addi rd, rs, 0
    if (MI.getOpcode() == RVTrim::ADD_UW &&
        MI.getOperand(1).getReg() == RVTrim::X0) {
      MI.removeOperand(1);
      MI.addOperand(MachineOperand::CreateImm(0));
      MI.setDesc(get(RVTrim::ADDI));
    }
    break;
  case RVTrim::SLTIU:
    // sltiu rd, zero, NZC => addi rd, zero, 1
    // sltiu rd, zero, 0 => addi rd, zero, 0
    if (MI.getOperand(1).getReg() == RVTrim::X0) {
      MI.getOperand(2).setImm(MI.getOperand(2).getImm() != 0);
      MI.setDesc(get(RVTrim::ADDI));
      return true;
    }
    break;
  case RVTrim::SEXT_H:
  case RVTrim::SEXT_B:
  case RVTrim::ZEXT_H_RV32:
  case RVTrim::ZEXT_H_RV64:
    // sext.[hb] rd, zero => addi rd, zero, 0
    // zext.h rd, zero => addi rd, zero, 0
    if (MI.getOperand(1).getReg() == RVTrim::X0) {
      MI.addOperand(MachineOperand::CreateImm(0));
      MI.setDesc(get(RVTrim::ADDI));
      return true;
    }
    break;
  case RVTrim::MIN:
  case RVTrim::MINU:
  case RVTrim::MAX:
  case RVTrim::MAXU:
    // min|max rd, rs, rs => addi rd, rs, 0
    if (MI.getOperand(1).getReg() == MI.getOperand(2).getReg()) {
      MI.getOperand(2).ChangeToImmediate(0);
      MI.setDesc(get(RVTrim::ADDI));
      return true;
    }
    break;
  case RVTrim::BEQ:
  case RVTrim::BNE:
    // b{eq,ne} zero, rs, imm => b{eq,ne} rs, zero, imm
    if (MI.getOperand(0).getReg() == RVTrim::X0) {
      MachineOperand MO0 = MI.getOperand(0);
      MI.removeOperand(0);
      MI.insert(MI.operands_begin() + 1, {MO0});
    }
    break;
  case RVTrim::BLTU:
    // bltu zero, rs, imm => bne rs, zero, imm
    if (MI.getOperand(0).getReg() == RVTrim::X0) {
      MachineOperand MO0 = MI.getOperand(0);
      MI.removeOperand(0);
      MI.insert(MI.operands_begin() + 1, {MO0});
      MI.setDesc(get(RVTrim::BNE));
    }
    break;
  case RVTrim::BGEU:
    // bgeu zero, rs, imm => beq rs, zero, imm
    if (MI.getOperand(0).getReg() == RVTrim::X0) {
      MachineOperand MO0 = MI.getOperand(0);
      MI.removeOperand(0);
      MI.insert(MI.operands_begin() + 1, {MO0});
      MI.setDesc(get(RVTrim::BEQ));
    }
    break;
  }
  return false;
}

// clang-format off
#define CASE_WIDEOP_OPCODE_COMMON(OP, LMUL)                                    \
  RVTrim::PseudoV##OP##_##LMUL##_TIED

#define CASE_WIDEOP_OPCODE_LMULS(OP)                                           \
  CASE_WIDEOP_OPCODE_COMMON(OP, MF8):                                          \
  case CASE_WIDEOP_OPCODE_COMMON(OP, MF4):                                     \
  case CASE_WIDEOP_OPCODE_COMMON(OP, MF2):                                     \
  case CASE_WIDEOP_OPCODE_COMMON(OP, M1):                                      \
  case CASE_WIDEOP_OPCODE_COMMON(OP, M2):                                      \
  case CASE_WIDEOP_OPCODE_COMMON(OP, M4)

#define CASE_WIDEOP_CHANGE_OPCODE_COMMON(OP, LMUL)                             \
  case RVTrim::PseudoV##OP##_##LMUL##_TIED:                                     \
    NewOpc = RVTrim::PseudoV##OP##_##LMUL;                                      \
    break;

#define CASE_WIDEOP_CHANGE_OPCODE_LMULS(OP)                                    \
  CASE_WIDEOP_CHANGE_OPCODE_COMMON(OP, MF8)                                    \
  CASE_WIDEOP_CHANGE_OPCODE_COMMON(OP, MF4)                                    \
  CASE_WIDEOP_CHANGE_OPCODE_COMMON(OP, MF2)                                    \
  CASE_WIDEOP_CHANGE_OPCODE_COMMON(OP, M1)                                     \
  CASE_WIDEOP_CHANGE_OPCODE_COMMON(OP, M2)                                     \
  CASE_WIDEOP_CHANGE_OPCODE_COMMON(OP, M4)

// FP Widening Ops may by SEW aware. Create SEW aware cases for these cases.
#define CASE_FP_WIDEOP_OPCODE_COMMON(OP, LMUL, SEW)                            \
  RVTrim::PseudoV##OP##_##LMUL##_##SEW##_TIED

#define CASE_FP_WIDEOP_OPCODE_LMULS(OP)                                        \
  CASE_FP_WIDEOP_OPCODE_COMMON(OP, MF4, E16):                                  \
  case CASE_FP_WIDEOP_OPCODE_COMMON(OP, MF2, E16):                             \
  case CASE_FP_WIDEOP_OPCODE_COMMON(OP, MF2, E32):                             \
  case CASE_FP_WIDEOP_OPCODE_COMMON(OP, M1, E16):                              \
  case CASE_FP_WIDEOP_OPCODE_COMMON(OP, M1, E32):                              \
  case CASE_FP_WIDEOP_OPCODE_COMMON(OP, M2, E16):                              \
  case CASE_FP_WIDEOP_OPCODE_COMMON(OP, M2, E32):                              \
  case CASE_FP_WIDEOP_OPCODE_COMMON(OP, M4, E16):                              \
  case CASE_FP_WIDEOP_OPCODE_COMMON(OP, M4, E32)                               \

#define CASE_FP_WIDEOP_CHANGE_OPCODE_COMMON(OP, LMUL, SEW)                     \
  case RVTrim::PseudoV##OP##_##LMUL##_##SEW##_TIED:                             \
    NewOpc = RVTrim::PseudoV##OP##_##LMUL##_##SEW;                              \
    break;

#define CASE_FP_WIDEOP_CHANGE_OPCODE_LMULS(OP)                                 \
  CASE_FP_WIDEOP_CHANGE_OPCODE_COMMON(OP, MF4, E16)                            \
  CASE_FP_WIDEOP_CHANGE_OPCODE_COMMON(OP, MF2, E16)                            \
  CASE_FP_WIDEOP_CHANGE_OPCODE_COMMON(OP, MF2, E32)                            \
  CASE_FP_WIDEOP_CHANGE_OPCODE_COMMON(OP, M1, E16)                             \
  CASE_FP_WIDEOP_CHANGE_OPCODE_COMMON(OP, M1, E32)                             \
  CASE_FP_WIDEOP_CHANGE_OPCODE_COMMON(OP, M2, E16)                             \
  CASE_FP_WIDEOP_CHANGE_OPCODE_COMMON(OP, M2, E32)                             \
  CASE_FP_WIDEOP_CHANGE_OPCODE_COMMON(OP, M4, E16)                             \
  CASE_FP_WIDEOP_CHANGE_OPCODE_COMMON(OP, M4, E32)                             \

#define CASE_FP_WIDEOP_OPCODE_LMULS_ALT(OP)                                    \
  CASE_FP_WIDEOP_OPCODE_COMMON(OP, MF4, E16):                                  \
  case CASE_FP_WIDEOP_OPCODE_COMMON(OP, MF2, E16):                             \
  case CASE_FP_WIDEOP_OPCODE_COMMON(OP, M1, E16):                              \
  case CASE_FP_WIDEOP_OPCODE_COMMON(OP, M2, E16):                              \
  case CASE_FP_WIDEOP_OPCODE_COMMON(OP, M4, E16)

#define CASE_FP_WIDEOP_CHANGE_OPCODE_LMULS_ALT(OP)                             \
  CASE_FP_WIDEOP_CHANGE_OPCODE_COMMON(OP, MF4, E16)                            \
  CASE_FP_WIDEOP_CHANGE_OPCODE_COMMON(OP, MF2, E16)                            \
  CASE_FP_WIDEOP_CHANGE_OPCODE_COMMON(OP, M1, E16)                             \
  CASE_FP_WIDEOP_CHANGE_OPCODE_COMMON(OP, M2, E16)                             \
  CASE_FP_WIDEOP_CHANGE_OPCODE_COMMON(OP, M4, E16)
// clang-format on

MachineInstr *RVTrimInstrInfo::convertToThreeAddress(MachineInstr &MI,
                                                    LiveIntervals *LIS) const {
  MachineInstrBuilder MIB;
  switch (MI.getOpcode()) {
  default:
    return nullptr;
  case CASE_FP_WIDEOP_OPCODE_LMULS_ALT(FWADD_ALT_WV):
  case CASE_FP_WIDEOP_OPCODE_LMULS_ALT(FWSUB_ALT_WV):
  case CASE_FP_WIDEOP_OPCODE_LMULS(FWADD_WV):
  case CASE_FP_WIDEOP_OPCODE_LMULS(FWSUB_WV): {
    assert(RVTrimII::hasVecPolicyOp(MI.getDesc().TSFlags) &&
           MI.getNumExplicitOperands() == 7 &&
           "Expect 7 explicit operands rd, rs2, rs1, rm, vl, sew, policy");
    // If the tail policy is undisturbed we can't convert.
    if ((MI.getOperand(RVTrimII::getVecPolicyOpNum(MI.getDesc())).getImm() &
         1) == 0)
      return nullptr;
    // clang-format off
    unsigned NewOpc;
    switch (MI.getOpcode()) {
    default:
      llvm_unreachable("Unexpected opcode");
    CASE_FP_WIDEOP_CHANGE_OPCODE_LMULS(FWADD_WV)
    CASE_FP_WIDEOP_CHANGE_OPCODE_LMULS(FWSUB_WV)
    CASE_FP_WIDEOP_CHANGE_OPCODE_LMULS_ALT(FWADD_ALT_WV)
    CASE_FP_WIDEOP_CHANGE_OPCODE_LMULS_ALT(FWSUB_ALT_WV)
    }
    // clang-format on

    MachineBasicBlock &MBB = *MI.getParent();
    MIB = BuildMI(MBB, MI, MI.getDebugLoc(), get(NewOpc))
              .add(MI.getOperand(0))
              .addReg(MI.getOperand(0).getReg(), RegState::Undef)
              .add(MI.getOperand(1))
              .add(MI.getOperand(2))
              .add(MI.getOperand(3))
              .add(MI.getOperand(4))
              .add(MI.getOperand(5))
              .add(MI.getOperand(6));
    break;
  }
  case CASE_WIDEOP_OPCODE_LMULS(WADD_WV):
  case CASE_WIDEOP_OPCODE_LMULS(WADDU_WV):
  case CASE_WIDEOP_OPCODE_LMULS(WSUB_WV):
  case CASE_WIDEOP_OPCODE_LMULS(WSUBU_WV): {
    // If the tail policy is undisturbed we can't convert.
    assert(RVTrimII::hasVecPolicyOp(MI.getDesc().TSFlags) &&
           MI.getNumExplicitOperands() == 6);
    if ((MI.getOperand(RVTrimII::getVecPolicyOpNum(MI.getDesc())).getImm() &
         1) == 0)
      return nullptr;

    // clang-format off
    unsigned NewOpc;
    switch (MI.getOpcode()) {
    default:
      llvm_unreachable("Unexpected opcode");
    CASE_WIDEOP_CHANGE_OPCODE_LMULS(WADD_WV)
    CASE_WIDEOP_CHANGE_OPCODE_LMULS(WADDU_WV)
    CASE_WIDEOP_CHANGE_OPCODE_LMULS(WSUB_WV)
    CASE_WIDEOP_CHANGE_OPCODE_LMULS(WSUBU_WV)
    }
    // clang-format on

    MachineBasicBlock &MBB = *MI.getParent();
    MIB = BuildMI(MBB, MI, MI.getDebugLoc(), get(NewOpc))
              .add(MI.getOperand(0))
              .addReg(MI.getOperand(0).getReg(), RegState::Undef)
              .add(MI.getOperand(1))
              .add(MI.getOperand(2))
              .add(MI.getOperand(3))
              .add(MI.getOperand(4))
              .add(MI.getOperand(5));
    break;
  }
  }
  MIB.copyImplicitOps(MI);

  if (LIS) {
    SlotIndex Idx = LIS->ReplaceMachineInstrInMaps(MI, *MIB);

    if (MI.getOperand(0).isEarlyClobber()) {
      // Use operand 1 was tied to early-clobber def operand 0, so its live
      // interval could have ended at an early-clobber slot. Now they are not
      // tied we need to update it to the normal register slot.
      LiveInterval &LI = LIS->getInterval(MI.getOperand(1).getReg());
      LiveRange::Segment *S = LI.getSegmentContaining(Idx);
      if (S->end == Idx.getRegSlot(true))
        S->end = Idx.getRegSlot();
    }
  }

  return MIB;
}

#undef CASE_WIDEOP_OPCODE_COMMON
#undef CASE_WIDEOP_OPCODE_LMULS
#undef CASE_WIDEOP_CHANGE_OPCODE_COMMON
#undef CASE_WIDEOP_CHANGE_OPCODE_LMULS
#undef CASE_FP_WIDEOP_OPCODE_COMMON
#undef CASE_FP_WIDEOP_OPCODE_LMULS
#undef CASE_FP_WIDEOP_CHANGE_OPCODE_COMMON
#undef CASE_FP_WIDEOP_CHANGE_OPCODE_LMULS

void RVTrimInstrInfo::mulImm(MachineFunction &MF, MachineBasicBlock &MBB,
                            MachineBasicBlock::iterator II, const DebugLoc &DL,
                            Register DestReg, uint32_t Amount,
                            MachineInstr::MIFlag Flag) const {
  MachineRegisterInfo &MRI = MF.getRegInfo();
  if (llvm::has_single_bit(Amount)) {
    uint32_t ShiftAmount = Log2_32(Amount);
    if (ShiftAmount == 0)
      return;
    BuildMI(MBB, II, DL, get(RVTrim::SLLI), DestReg)
        .addReg(DestReg, RegState::Kill)
        .addImm(ShiftAmount)
        .setMIFlag(Flag);
  } else if (int ShXAmount, ShiftAmount;
             STI.hasShlAdd(3) &&
             (ShXAmount = isShifted359(Amount, ShiftAmount)) != 0) {
    // We can use Zba SHXADD+SLLI instructions for multiply in some cases.
    unsigned Opc;
    switch (ShXAmount) {
    case 1:
      Opc = RVTrim::SH1ADD;
      break;
    case 2:
      Opc = RVTrim::SH2ADD;
      break;
    case 3:
      Opc = RVTrim::SH3ADD;
      break;
    default:
      llvm_unreachable("unexpected result of isShifted359");
    }
    if (ShiftAmount)
      BuildMI(MBB, II, DL, get(RVTrim::SLLI), DestReg)
          .addReg(DestReg, RegState::Kill)
          .addImm(ShiftAmount)
          .setMIFlag(Flag);
    BuildMI(MBB, II, DL, get(Opc), DestReg)
        .addReg(DestReg, RegState::Kill)
        .addReg(DestReg)
        .setMIFlag(Flag);
  } else if (llvm::has_single_bit(Amount - 1)) {
    Register ScaledRegister = MRI.createVirtualRegister(&RVTrim::GPRRegClass);
    uint32_t ShiftAmount = Log2_32(Amount - 1);
    BuildMI(MBB, II, DL, get(RVTrim::SLLI), ScaledRegister)
        .addReg(DestReg)
        .addImm(ShiftAmount)
        .setMIFlag(Flag);
    BuildMI(MBB, II, DL, get(RVTrim::ADD), DestReg)
        .addReg(ScaledRegister, RegState::Kill)
        .addReg(DestReg, RegState::Kill)
        .setMIFlag(Flag);
  } else if (llvm::has_single_bit(Amount + 1)) {
    Register ScaledRegister = MRI.createVirtualRegister(&RVTrim::GPRRegClass);
    uint32_t ShiftAmount = Log2_32(Amount + 1);
    BuildMI(MBB, II, DL, get(RVTrim::SLLI), ScaledRegister)
        .addReg(DestReg)
        .addImm(ShiftAmount)
        .setMIFlag(Flag);
    BuildMI(MBB, II, DL, get(RVTrim::SUB), DestReg)
        .addReg(ScaledRegister, RegState::Kill)
        .addReg(DestReg, RegState::Kill)
        .setMIFlag(Flag);
  } else if (STI.hasStdExtZmmul()) {
    Register N = MRI.createVirtualRegister(&RVTrim::GPRRegClass);
    movImm(MBB, II, DL, N, Amount, Flag);
    BuildMI(MBB, II, DL, get(RVTrim::MUL), DestReg)
        .addReg(DestReg, RegState::Kill)
        .addReg(N, RegState::Kill)
        .setMIFlag(Flag);
  } else {
    Register Acc;
    uint32_t PrevShiftAmount = 0;
    for (uint32_t ShiftAmount = 0; Amount >> ShiftAmount; ShiftAmount++) {
      if (Amount & (1U << ShiftAmount)) {
        if (ShiftAmount)
          BuildMI(MBB, II, DL, get(RVTrim::SLLI), DestReg)
              .addReg(DestReg, RegState::Kill)
              .addImm(ShiftAmount - PrevShiftAmount)
              .setMIFlag(Flag);
        if (Amount >> (ShiftAmount + 1)) {
          // If we don't have an accmulator yet, create it and copy DestReg.
          if (!Acc) {
            Acc = MRI.createVirtualRegister(&RVTrim::GPRRegClass);
            BuildMI(MBB, II, DL, get(TargetOpcode::COPY), Acc)
                .addReg(DestReg)
                .setMIFlag(Flag);
          } else {
            BuildMI(MBB, II, DL, get(RVTrim::ADD), Acc)
                .addReg(Acc, RegState::Kill)
                .addReg(DestReg)
                .setMIFlag(Flag);
          }
        }
        PrevShiftAmount = ShiftAmount;
      }
    }
    assert(Acc && "Expected valid accumulator");
    BuildMI(MBB, II, DL, get(RVTrim::ADD), DestReg)
        .addReg(DestReg, RegState::Kill)
        .addReg(Acc, RegState::Kill)
        .setMIFlag(Flag);
  }
}

ArrayRef<std::pair<MachineMemOperand::Flags, const char *>>
RVTrimInstrInfo::getSerializableMachineMemOperandTargetFlags() const {
  static const std::pair<MachineMemOperand::Flags, const char *> TargetFlags[] =
      {{MONontemporalBit0, "rvtrim-nontemporal-domain-bit-0"},
       {MONontemporalBit1, "rvtrim-nontemporal-domain-bit-1"}};
  return ArrayRef(TargetFlags);
}

unsigned RVTrimInstrInfo::getTailDuplicateSize(CodeGenOptLevel OptLevel) const {
  return OptLevel >= CodeGenOptLevel::Aggressive
             ? STI.getTailDupAggressiveThreshold()
             : 2;
}

bool RVTrim::isRVVSpill(const MachineInstr &MI) {
  // RVV lacks any support for immediate addressing for stack addresses, so be
  // conservative.
  unsigned Opcode = MI.getOpcode();
  if (!RVTrimVPseudosTable::getPseudoInfo(Opcode) &&
      !getLMULForRVVWholeLoadStore(Opcode) && !isRVVSpillForZvlsseg(Opcode))
    return false;
  return true;
}

/// Return true if \p MI is a copy that will be lowered to one or more vmvNr.vs.
bool RVTrim::isVectorCopy(const TargetRegisterInfo *TRI,
                         const MachineInstr &MI) {
  return MI.isCopy() && MI.getOperand(0).getReg().isPhysical() &&
         RVTrimRegisterInfo::isRVVRegClass(
             TRI->getMinimalPhysRegClass(MI.getOperand(0).getReg()));
}

std::optional<std::pair<unsigned, unsigned>>
RVTrim::isRVVSpillForZvlsseg(unsigned Opcode) {
  switch (Opcode) {
  default:
    return std::nullopt;
  case RVTrim::PseudoVSPILL2_M1:
  case RVTrim::PseudoVRELOAD2_M1:
    return std::make_pair(2u, 1u);
  case RVTrim::PseudoVSPILL2_M2:
  case RVTrim::PseudoVRELOAD2_M2:
    return std::make_pair(2u, 2u);
  case RVTrim::PseudoVSPILL2_M4:
  case RVTrim::PseudoVRELOAD2_M4:
    return std::make_pair(2u, 4u);
  case RVTrim::PseudoVSPILL3_M1:
  case RVTrim::PseudoVRELOAD3_M1:
    return std::make_pair(3u, 1u);
  case RVTrim::PseudoVSPILL3_M2:
  case RVTrim::PseudoVRELOAD3_M2:
    return std::make_pair(3u, 2u);
  case RVTrim::PseudoVSPILL4_M1:
  case RVTrim::PseudoVRELOAD4_M1:
    return std::make_pair(4u, 1u);
  case RVTrim::PseudoVSPILL4_M2:
  case RVTrim::PseudoVRELOAD4_M2:
    return std::make_pair(4u, 2u);
  case RVTrim::PseudoVSPILL5_M1:
  case RVTrim::PseudoVRELOAD5_M1:
    return std::make_pair(5u, 1u);
  case RVTrim::PseudoVSPILL6_M1:
  case RVTrim::PseudoVRELOAD6_M1:
    return std::make_pair(6u, 1u);
  case RVTrim::PseudoVSPILL7_M1:
  case RVTrim::PseudoVRELOAD7_M1:
    return std::make_pair(7u, 1u);
  case RVTrim::PseudoVSPILL8_M1:
  case RVTrim::PseudoVRELOAD8_M1:
    return std::make_pair(8u, 1u);
  }
}

bool RVTrim::hasEqualFRM(const MachineInstr &MI1, const MachineInstr &MI2) {
  int16_t MI1FrmOpIdx =
      RVTrim::getNamedOperandIdx(MI1.getOpcode(), RVTrim::OpName::frm);
  int16_t MI2FrmOpIdx =
      RVTrim::getNamedOperandIdx(MI2.getOpcode(), RVTrim::OpName::frm);
  if (MI1FrmOpIdx < 0 || MI2FrmOpIdx < 0)
    return false;
  MachineOperand FrmOp1 = MI1.getOperand(MI1FrmOpIdx);
  MachineOperand FrmOp2 = MI2.getOperand(MI2FrmOpIdx);
  return FrmOp1.getImm() == FrmOp2.getImm();
}

std::optional<unsigned>
RVTrim::getVectorLowDemandedScalarBits(unsigned Opcode, unsigned Log2SEW) {
  switch (Opcode) {
  default:
    return std::nullopt;

  // 11.6. Vector Single-Width Shift Instructions
  case RVTrim::VSLL_VX:
  case RVTrim::VSRL_VX:
  case RVTrim::VSRA_VX:
  // 12.4. Vector Single-Width Scaling Shift Instructions
  case RVTrim::VSSRL_VX:
  case RVTrim::VSSRA_VX:
  // Zvbb
  case RVTrim::VROL_VX:
  case RVTrim::VROR_VX:
    // Only the low lg2(SEW) bits of the shift-amount value are used.
    return Log2SEW;

  // 11.7 Vector Narrowing Integer Right Shift Instructions
  case RVTrim::VNSRL_WX:
  case RVTrim::VNSRA_WX:
  // 12.5. Vector Narrowing Fixed-Point Clip Instructions
  case RVTrim::VNCLIPU_WX:
  case RVTrim::VNCLIP_WX:
  // Zvbb
  case RVTrim::VWSLL_VX:
    // Only the low lg2(2*SEW) bits of the shift-amount value are used.
    return Log2SEW + 1;

  // 11.1. Vector Single-Width Integer Add and Subtract
  case RVTrim::VADD_VX:
  case RVTrim::VSUB_VX:
  case RVTrim::VRSUB_VX:
  // 11.2. Vector Widening Integer Add/Subtract
  case RVTrim::VWADDU_VX:
  case RVTrim::VWSUBU_VX:
  case RVTrim::VWADD_VX:
  case RVTrim::VWSUB_VX:
  case RVTrim::VWADDU_WX:
  case RVTrim::VWSUBU_WX:
  case RVTrim::VWADD_WX:
  case RVTrim::VWSUB_WX:
  // 11.4. Vector Integer Add-with-Carry / Subtract-with-Borrow Instructions
  case RVTrim::VADC_VXM:
  case RVTrim::VADC_VIM:
  case RVTrim::VMADC_VXM:
  case RVTrim::VMADC_VIM:
  case RVTrim::VMADC_VX:
  case RVTrim::VSBC_VXM:
  case RVTrim::VMSBC_VXM:
  case RVTrim::VMSBC_VX:
  // 11.5 Vector Bitwise Logical Instructions
  case RVTrim::VAND_VX:
  case RVTrim::VOR_VX:
  case RVTrim::VXOR_VX:
  // 11.8. Vector Integer Compare Instructions
  case RVTrim::VMSEQ_VX:
  case RVTrim::VMSNE_VX:
  case RVTrim::VMSLTU_VX:
  case RVTrim::VMSLT_VX:
  case RVTrim::VMSLEU_VX:
  case RVTrim::VMSLE_VX:
  case RVTrim::VMSGTU_VX:
  case RVTrim::VMSGT_VX:
  // 11.9. Vector Integer Min/Max Instructions
  case RVTrim::VMINU_VX:
  case RVTrim::VMIN_VX:
  case RVTrim::VMAXU_VX:
  case RVTrim::VMAX_VX:
  // 11.10. Vector Single-Width Integer Multiply Instructions
  case RVTrim::VMUL_VX:
  case RVTrim::VMULH_VX:
  case RVTrim::VMULHU_VX:
  case RVTrim::VMULHSU_VX:
  // 11.11. Vector Integer Divide Instructions
  case RVTrim::VDIVU_VX:
  case RVTrim::VDIV_VX:
  case RVTrim::VREMU_VX:
  case RVTrim::VREM_VX:
  // 11.12. Vector Widening Integer Multiply Instructions
  case RVTrim::VWMUL_VX:
  case RVTrim::VWMULU_VX:
  case RVTrim::VWMULSU_VX:
  // 11.13. Vector Single-Width Integer Multiply-Add Instructions
  case RVTrim::VMACC_VX:
  case RVTrim::VNMSAC_VX:
  case RVTrim::VMADD_VX:
  case RVTrim::VNMSUB_VX:
  // 11.14. Vector Widening Integer Multiply-Add Instructions
  case RVTrim::VWMACCU_VX:
  case RVTrim::VWMACC_VX:
  case RVTrim::VWMACCSU_VX:
  case RVTrim::VWMACCUS_VX:
  // 11.15. Vector Integer Merge Instructions
  case RVTrim::VMERGE_VXM:
  // 11.16. Vector Integer Move Instructions
  case RVTrim::VMV_V_X:
  // 12.1. Vector Single-Width Saturating Add and Subtract
  case RVTrim::VSADDU_VX:
  case RVTrim::VSADD_VX:
  case RVTrim::VSSUBU_VX:
  case RVTrim::VSSUB_VX:
  // 12.2. Vector Single-Width Averaging Add and Subtract
  case RVTrim::VAADDU_VX:
  case RVTrim::VAADD_VX:
  case RVTrim::VASUBU_VX:
  case RVTrim::VASUB_VX:
  // 12.3. Vector Single-Width Fractional Multiply with Rounding and Saturation
  case RVTrim::VSMUL_VX:
  // 16.1. Integer Scalar Move Instructions
  case RVTrim::VMV_S_X:
  // Zvbb
  case RVTrim::VANDN_VX:
    return 1U << Log2SEW;
  }
}

unsigned RVTrim::getRVVMCOpcode(unsigned RVVPseudoOpcode) {
  const RVTrimVPseudosTable::PseudoInfo *RVV =
      RVTrimVPseudosTable::getPseudoInfo(RVVPseudoOpcode);
  if (!RVV)
    return 0;
  return RVV->BaseInstr;
}

unsigned RVTrim::getDestLog2EEW(const MCInstrDesc &Desc, unsigned Log2SEW) {
  unsigned DestEEW =
      (Desc.TSFlags & RVTrimII::DestEEWMask) >> RVTrimII::DestEEWShift;
  // EEW = 1
  if (DestEEW == 0)
    return 0;
  // EEW = SEW * n
  unsigned Scaled = Log2SEW + (DestEEW - 1);
  assert(Scaled >= 3 && Scaled <= 6);
  return Scaled;
}

static std::optional<int64_t> getEffectiveImm(const MachineRegisterInfo &MRI,
                                              const MachineOperand &MO) {
  assert(MO.isImm() || MO.getReg().isVirtual());
  if (MO.isImm())
    return MO.getImm();
  const MachineInstr *Def = MRI.getVRegDef(MO.getReg());
  int64_t Imm;
  if (isLoadImm(Def, Imm))
    return Imm;
  return std::nullopt;
}

/// Given two VL operands, do we know that LHS <= RHS? Must be used in SSA form.
bool RVTrim::isVLKnownLE(const MachineRegisterInfo &MRI,
                        const MachineOperand &LHS, const MachineOperand &RHS) {
  assert((LHS.isImm() || MRI.isSSA()) && (RHS.isImm() || MRI.isSSA()));
  if (LHS.isReg() && RHS.isReg() && LHS.getReg().isVirtual() &&
      LHS.getReg() == RHS.getReg())
    return true;
  if (RHS.isImm() && RHS.getImm() == RVTrim::VLMaxSentinel)
    return true;
  if (LHS.isImm() && LHS.getImm() == 0)
    return true;
  if (LHS.isImm() && LHS.getImm() == RVTrim::VLMaxSentinel)
    return false;
  std::optional<int64_t> LHSImm = getEffectiveImm(MRI, LHS),
                         RHSImm = getEffectiveImm(MRI, RHS);
  if (!LHSImm || !RHSImm)
    return false;
  return LHSImm <= RHSImm;
}

namespace {
class RVTrimPipelinerLoopInfo : public TargetInstrInfo::PipelinerLoopInfo {
  const MachineInstr *LHS;
  const MachineInstr *RHS;
  SmallVector<MachineOperand, 3> Cond;

public:
  RVTrimPipelinerLoopInfo(const MachineInstr *LHS, const MachineInstr *RHS,
                         const SmallVectorImpl<MachineOperand> &Cond)
      : LHS(LHS), RHS(RHS), Cond(Cond.begin(), Cond.end()) {}

  bool shouldIgnoreForPipelining(const MachineInstr *MI) const override {
    // Make the instructions for loop control be placed in stage 0.
    // The predecessors of LHS/RHS are considered by the caller.
    if (LHS && MI == LHS)
      return true;
    if (RHS && MI == RHS)
      return true;
    return false;
  }

  std::optional<bool> createTripCountGreaterCondition(
      int TC, MachineBasicBlock &MBB,
      SmallVectorImpl<MachineOperand> &CondParam) override {
    // A branch instruction will be inserted as "if (Cond) goto epilogue".
    // Cond is normalized for such use.
    // The predecessors of the branch are assumed to have already been inserted.
    CondParam = Cond;
    return {};
  }

  void setPreheader(MachineBasicBlock *NewPreheader) override {}

  void adjustTripCount(int TripCountAdjust) override {}
};
} // namespace

std::unique_ptr<TargetInstrInfo::PipelinerLoopInfo>
RVTrimInstrInfo::analyzeLoopForPipelining(MachineBasicBlock *LoopBB) const {
  MachineBasicBlock *TBB = nullptr, *FBB = nullptr;
  SmallVector<MachineOperand, 4> Cond;
  if (analyzeBranch(*LoopBB, TBB, FBB, Cond, /*AllowModify=*/false))
    return nullptr;

  // Infinite loops are not supported
  if (TBB == LoopBB && FBB == LoopBB)
    return nullptr;

  // Must be conditional branch
  if (FBB == nullptr)
    return nullptr;

  assert((TBB == LoopBB || FBB == LoopBB) &&
         "The Loop must be a single-basic-block loop");

  // Normalization for createTripCountGreaterCondition()
  if (TBB == LoopBB)
    reverseBranchCondition(Cond);

  const MachineRegisterInfo &MRI = LoopBB->getParent()->getRegInfo();
  auto FindRegDef = [&MRI](MachineOperand &Op) -> const MachineInstr * {
    if (!Op.isReg())
      return nullptr;
    Register Reg = Op.getReg();
    if (!Reg.isVirtual())
      return nullptr;
    return MRI.getVRegDef(Reg);
  };

  const MachineInstr *LHS = FindRegDef(Cond[1]);
  const MachineInstr *RHS = FindRegDef(Cond[2]);
  if (LHS && LHS->isPHI())
    return nullptr;
  if (RHS && RHS->isPHI())
    return nullptr;

  return std::make_unique<RVTrimPipelinerLoopInfo>(LHS, RHS, Cond);
}

// FIXME: We should remove this if we have a default generic scheduling model.
bool RVTrimInstrInfo::isHighLatencyDef(int Opc) const {
  unsigned RVVMCOpcode = RVTrim::getRVVMCOpcode(Opc);
  Opc = RVVMCOpcode ? RVVMCOpcode : Opc;
  switch (Opc) {
  default:
    return false;
  // Integer div/rem.
  case RVTrim::DIV:
  case RVTrim::DIVW:
  case RVTrim::DIVU:
  case RVTrim::DIVUW:
  case RVTrim::REM:
  case RVTrim::REMW:
  case RVTrim::REMU:
  case RVTrim::REMUW:
  // Floating-point div/sqrt.
  case RVTrim::FDIV_H:
  case RVTrim::FDIV_S:
  case RVTrim::FDIV_D:
  case RVTrim::FDIV_H_INX:
  case RVTrim::FDIV_S_INX:
  case RVTrim::FDIV_D_INX:
  case RVTrim::FDIV_D_IN32X:
  case RVTrim::FSQRT_H:
  case RVTrim::FSQRT_S:
  case RVTrim::FSQRT_D:
  case RVTrim::FSQRT_H_INX:
  case RVTrim::FSQRT_S_INX:
  case RVTrim::FSQRT_D_INX:
  case RVTrim::FSQRT_D_IN32X:
  // Vector integer div/rem
  case RVTrim::VDIV_VV:
  case RVTrim::VDIV_VX:
  case RVTrim::VDIVU_VV:
  case RVTrim::VDIVU_VX:
  case RVTrim::VREM_VV:
  case RVTrim::VREM_VX:
  case RVTrim::VREMU_VV:
  case RVTrim::VREMU_VX:
  // Vector floating-point div/sqrt.
  case RVTrim::VFDIV_VV:
  case RVTrim::VFDIV_VF:
  case RVTrim::VFRDIV_VF:
  case RVTrim::VFSQRT_V:
  case RVTrim::VFRSQRT7_V:
    return true;
  }
}

bool RVTrimInstrInfo::isVRegCopy(const MachineInstr *MI, unsigned LMul) const {
  if (MI->getOpcode() != TargetOpcode::COPY)
    return false;
  const MachineRegisterInfo &MRI = MI->getMF()->getRegInfo();
  const TargetRegisterInfo *TRI = MRI.getTargetRegisterInfo();

  Register DstReg = MI->getOperand(0).getReg();
  const TargetRegisterClass *RC = DstReg.isVirtual()
                                      ? MRI.getRegClass(DstReg)
                                      : TRI->getMinimalPhysRegClass(DstReg);

  if (!RVTrimRegisterInfo::isRVVRegClass(RC))
    return false;

  if (!LMul)
    return true;

  // TODO: Perhaps we could distinguish segment register classes (e.g. VRN3M2)
  // in the future.
  auto [RCLMul, RCFractional] =
      RISCVVType::decodeVLMUL(RVTrimRI::getLMul(RC->TSFlags));
  return (!RCFractional && LMul == RCLMul) || (RCFractional && LMul == 1);
}

bool RVTrimInstrInfo::requiresNTLHint(const MachineInstr &MI) const {
  if (MI.memoperands_empty())
    return false;

  MachineMemOperand *MMO = *(MI.memoperands_begin());
  if (!MMO->isNonTemporal())
    return false;

  return true;
}

bool RVTrimInstrInfo::isSafeToMove(const MachineInstr &From,
                                  const MachineBasicBlock::iterator &To) {
  assert(To == From.getParent()->end() || From.getParent() == To->getParent());
  SmallVector<Register> PhysUses, PhysDefs;
  for (const MachineOperand &MO : From.all_uses())
    if (MO.getReg().isPhysical())
      PhysUses.push_back(MO.getReg());
  for (const MachineOperand &MO : From.all_defs())
    if (MO.getReg().isPhysical())
      PhysDefs.push_back(MO.getReg());
  bool SawStore = false;
  for (auto II = std::next(From.getIterator()); II != To; II++) {
    for (Register PhysReg : PhysUses)
      if (II->definesRegister(PhysReg, nullptr))
        return false;
    for (Register PhysReg : PhysDefs)
      if (II->definesRegister(PhysReg, nullptr) ||
          II->readsRegister(PhysReg, nullptr))
        return false;
    II->isSafeToMove(SawStore);
    if (SawStore)
      break;
  }
  return From.isSafeToMove(SawStore);
}
