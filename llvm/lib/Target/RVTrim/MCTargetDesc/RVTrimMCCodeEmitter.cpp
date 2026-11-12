//===-- RVTrimMCCodeEmitter.cpp - Convert RISC-V code to machine code ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the RVTrimMCCodeEmitter class.
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/RVTrimBaseInfo.h"
#include "MCTargetDesc/RVTrimFixupKinds.h"
#include "MCTargetDesc/RVTrimMCAsmInfo.h"
#include "MCTargetDesc/RVTrimMCTargetDesc.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCCodeEmitter.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstBuilder.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/EndianStream.h"

using namespace llvm;

#define DEBUG_TYPE "mccodeemitter"

STATISTIC(MCNumEmitted, "Number of MC instructions emitted");
STATISTIC(MCNumFixups, "Number of MC fixups created");

namespace {
class RVTrimMCCodeEmitter : public MCCodeEmitter {
  RVTrimMCCodeEmitter(const RVTrimMCCodeEmitter &) = delete;
  void operator=(const RVTrimMCCodeEmitter &) = delete;
  MCContext &Ctx;
  MCInstrInfo const &MCII;

public:
  RVTrimMCCodeEmitter(MCContext &ctx, MCInstrInfo const &MCII)
      : Ctx(ctx), MCII(MCII) {}

  ~RVTrimMCCodeEmitter() override = default;

  void encodeInstruction(const MCInst &MI, SmallVectorImpl<char> &CB,
                         SmallVectorImpl<MCFixup> &Fixups,
                         const MCSubtargetInfo &STI) const override;

  void expandFunctionCall(const MCInst &MI, SmallVectorImpl<char> &CB,
                          SmallVectorImpl<MCFixup> &Fixups,
                          const MCSubtargetInfo &STI) const;

  void expandTLSDESCCall(const MCInst &MI, SmallVectorImpl<char> &CB,
                         SmallVectorImpl<MCFixup> &Fixups,
                         const MCSubtargetInfo &STI) const;

  void expandAddTPRel(const MCInst &MI, SmallVectorImpl<char> &CB,
                      SmallVectorImpl<MCFixup> &Fixups,
                      const MCSubtargetInfo &STI) const;

  void expandLongCondBr(const MCInst &MI, SmallVectorImpl<char> &CB,
                        SmallVectorImpl<MCFixup> &Fixups,
                        const MCSubtargetInfo &STI) const;

  void expandFunctionCallLpad(const MCInst &MI, SmallVectorImpl<char> &CB,
                              SmallVectorImpl<MCFixup> &Fixups,
                              const MCSubtargetInfo &STI) const;

  void expandQCLongCondBrImm(const MCInst &MI, SmallVectorImpl<char> &CB,
                             SmallVectorImpl<MCFixup> &Fixups,
                             const MCSubtargetInfo &STI, unsigned Size) const;

  void expandPseudoQCAccess(const MCInst &MI, SmallVectorImpl<char> &CB,
                            SmallVectorImpl<MCFixup> &Fixups,
                            const MCSubtargetInfo &STI) const;

  /// TableGen'erated function for getting the binary encoding for an
  /// instruction.
  uint64_t getBinaryCodeForInstr(const MCInst &MI,
                                 SmallVectorImpl<MCFixup> &Fixups,
                                 const MCSubtargetInfo &STI) const;

  /// Return binary encoding of operand. If the machine operand requires
  /// relocation, record the relocation and return zero.
  uint64_t getMachineOpValue(const MCInst &MI, const MCOperand &MO,
                             SmallVectorImpl<MCFixup> &Fixups,
                             const MCSubtargetInfo &STI) const;

  uint64_t getImmOpValueMinus1(const MCInst &MI, unsigned OpNo,
                               SmallVectorImpl<MCFixup> &Fixups,
                               const MCSubtargetInfo &STI) const;

  uint64_t getImmOpValueSlist(const MCInst &MI, unsigned OpNo,
                              SmallVectorImpl<MCFixup> &Fixups,
                              const MCSubtargetInfo &STI) const;

  template <unsigned N>
  unsigned getImmOpValueAsrN(const MCInst &MI, unsigned OpNo,
                             SmallVectorImpl<MCFixup> &Fixups,
                             const MCSubtargetInfo &STI) const;

  uint64_t getImmOpValueZibi(const MCInst &MI, unsigned OpNo,
                             SmallVectorImpl<MCFixup> &Fixups,
                             const MCSubtargetInfo &STI) const;

  uint64_t getImmOpValue(const MCInst &MI, unsigned OpNo,
                         SmallVectorImpl<MCFixup> &Fixups,
                         const MCSubtargetInfo &STI) const;

  unsigned getYBNDSWImmOpValue(const MCInst &MI, unsigned OpNo,
                               SmallVectorImpl<MCFixup> &Fixups,
                               const MCSubtargetInfo &STI) const;

  unsigned getVMaskReg(const MCInst &MI, unsigned OpNo,
                       SmallVectorImpl<MCFixup> &Fixups,
                       const MCSubtargetInfo &STI) const;

  unsigned getRlistOpValue(const MCInst &MI, unsigned OpNo,
                           SmallVectorImpl<MCFixup> &Fixups,
                           const MCSubtargetInfo &STI) const;

  unsigned getRlistS0OpValue(const MCInst &MI, unsigned OpNo,
                             SmallVectorImpl<MCFixup> &Fixups,
                             const MCSubtargetInfo &STI) const;
};
} // end anonymous namespace

MCCodeEmitter *llvm::createRVTrimMCCodeEmitter(const MCInstrInfo &MCII,
                                              MCContext &Ctx) {
  return new RVTrimMCCodeEmitter(Ctx, MCII);
}

static void addFixup(SmallVectorImpl<MCFixup> &Fixups, uint32_t Offset,
                     const MCExpr *Value, uint16_t Kind) {
  bool PCRel = false;
  switch (Kind) {
  case ELF::R_RISCV_CALL_PLT:
  case RVTrim::fixup_rvtrim_pcrel_hi20:
  case RVTrim::fixup_rvtrim_pcrel_lo12_i:
  case RVTrim::fixup_rvtrim_pcrel_lo12_s:
  case RVTrim::fixup_rvtrim_jal:
  case RVTrim::fixup_rvtrim_branch:
  case RVTrim::fixup_rvtrim_rvc_jump:
  case RVTrim::fixup_rvtrim_rvc_branch:
  case RVTrim::fixup_rvtrim_call:
  case RVTrim::fixup_rvtrim_call_plt:
  case RVTrim::fixup_rvtrim_qc_e_branch:
  case RVTrim::fixup_rvtrim_qc_e_call_plt:
  case RVTrim::fixup_rvtrim_nds_branch_10:
    PCRel = true;
  }
  Fixups.push_back(MCFixup::create(Offset, Value, Kind, PCRel));
}

// Expand PseudoCALL(Reg), PseudoTAIL and PseudoJump to AUIPC and JALR with
// relocation types. We expand those pseudo-instructions while encoding them,
// meaning AUIPC and JALR won't go through RISC-V MC to MC compressed
// instruction transformation. This is acceptable because AUIPC has no 16-bit
// form and C_JALR has no immediate operand field.  We let linker relaxation
// deal with it. When linker relaxation is enabled, AUIPC and JALR have a
// chance to relax to JAL.
// If the C extension is enabled, JAL has a chance relax to C_JAL.
void RVTrimMCCodeEmitter::expandFunctionCall(const MCInst &MI,
                                            SmallVectorImpl<char> &CB,
                                            SmallVectorImpl<MCFixup> &Fixups,
                                            const MCSubtargetInfo &STI) const {
  MCInst TmpInst;
  MCOperand Func;
  MCRegister Ra;
  if (MI.getOpcode() == RVTrim::PseudoTAIL) {
    Func = MI.getOperand(0);
    Ra = RVTrimII::getTailExpandUseRegNo(STI.getFeatureBits());
  } else if (MI.getOpcode() == RVTrim::PseudoTAILReg) {
    Func = MI.getOperand(0);
    Ra = MI.getOperand(1).getReg();
  } else if (MI.getOpcode() == RVTrim::PseudoCALLReg) {
    Func = MI.getOperand(1);
    Ra = MI.getOperand(0).getReg();
  } else if (MI.getOpcode() == RVTrim::PseudoCALL) {
    Func = MI.getOperand(0);
    Ra = RVTrim::X1;
  } else if (MI.getOpcode() == RVTrim::PseudoJump) {
    Func = MI.getOperand(1);
    Ra = MI.getOperand(0).getReg();
  }
  uint32_t Binary;

  assert(Func.isExpr() && "Expected expression");

  const MCExpr *CallExpr = Func.getExpr();

  if (STI.getTargetTriple().isOSBinFormatMachO()) {
    MCOperand FuncOp = MCOperand::createExpr(CallExpr);
    if (MI.getOpcode() == RVTrim::PseudoTAIL ||
        MI.getOpcode() == RVTrim::PseudoTAILReg ||
        MI.getOpcode() == RVTrim::PseudoJump)
      // Emit JAL X0, Func
      TmpInst = MCInstBuilder(RVTrim::JAL).addReg(RVTrim::X0).addOperand(FuncOp);
    else
      // Emit JAL Ra, Func
      TmpInst = MCInstBuilder(RVTrim::JAL).addReg(Ra).addOperand(FuncOp);
    Binary = getBinaryCodeForInstr(TmpInst, Fixups, STI);
    support::endian::write(CB, Binary, llvm::endianness::little);
    return;
  }
  // Emit AUIPC Ra, Func with R_RISCV_CALL relocation type.
  TmpInst = MCInstBuilder(RVTrim::AUIPC).addReg(Ra).addExpr(CallExpr);
  Binary = getBinaryCodeForInstr(TmpInst, Fixups, STI);
  support::endian::write(CB, Binary, llvm::endianness::little);

  if (MI.getOpcode() == RVTrim::PseudoTAIL ||
      MI.getOpcode() == RVTrim::PseudoTAILReg ||
      MI.getOpcode() == RVTrim::PseudoJump)
    // Emit JALR X0, Ra, 0
    TmpInst = MCInstBuilder(RVTrim::JALR).addReg(RVTrim::X0).addReg(Ra).addImm(0);
  else
    // Emit JALR Ra, Ra, 0
    TmpInst = MCInstBuilder(RVTrim::JALR).addReg(Ra).addReg(Ra).addImm(0);
  Binary = getBinaryCodeForInstr(TmpInst, Fixups, STI);
  support::endian::write(CB, Binary, llvm::endianness::little);
}

// Expand to AUIPC+JALR+LPAD (direct) or JALR+LPAD (indirect).
// R_RISCV_RELAX is not emitted for the call because linker relaxation could
// convert it to c.jal/cm.jalt, which would misalign the following LPAD.
void RVTrimMCCodeEmitter::expandFunctionCallLpad(
    const MCInst &MI, SmallVectorImpl<char> &CB,
    SmallVectorImpl<MCFixup> &Fixups, const MCSubtargetInfo &STI) const {
  bool IsIndirect = MI.getOpcode() == RVTrim::PseudoCALLIndirectLpadAlign;
  MCInst TmpInst;
  uint32_t Binary;

  if (!IsIndirect) {
    const MCOperand &Func = MI.getOperand(0);
    assert(Func.isExpr() && "Expected expression for call target");

    // Use a STI without FeatureRelax so getImmOpValue does not mark the
    // R_RISCV_CALL_PLT fixup as LinkerRelaxable (which would emit
    // R_RISCV_RELAX).
    MCSubtargetInfo NoRelaxSTI(STI);
    if (STI.hasFeature(RVTrim::FeatureRelax))
      NoRelaxSTI.ToggleFeature(RVTrim::FeatureRelax);

    TmpInst =
        MCInstBuilder(RVTrim::AUIPC).addReg(RVTrim::X1).addExpr(Func.getExpr());
    Binary = getBinaryCodeForInstr(TmpInst, Fixups, NoRelaxSTI);
    support::endian::write(CB, Binary, llvm::endianness::little);

    TmpInst = MCInstBuilder(RVTrim::JALR)
                  .addReg(RVTrim::X1)
                  .addReg(RVTrim::X1)
                  .addImm(0);
    Binary = getBinaryCodeForInstr(TmpInst, Fixups, STI);
    support::endian::write(CB, Binary, llvm::endianness::little);
  } else {
    TmpInst = MCInstBuilder(RVTrim::JALR)
                  .addReg(RVTrim::X1)
                  .addReg(MI.getOperand(0).getReg())
                  .addImm(0);
    Binary = getBinaryCodeForInstr(TmpInst, Fixups, STI);
    support::endian::write(CB, Binary, llvm::endianness::little);
  }

  // LPAD is encoded as AUIPC X0, label.
  TmpInst = MCInstBuilder(RVTrim::AUIPC)
                .addReg(RVTrim::X0)
                .addImm(MI.getOperand(1).getImm());
  Binary = getBinaryCodeForInstr(TmpInst, Fixups, STI);
  support::endian::write(CB, Binary, llvm::endianness::little);
}

void RVTrimMCCodeEmitter::expandTLSDESCCall(const MCInst &MI,
                                           SmallVectorImpl<char> &CB,
                                           SmallVectorImpl<MCFixup> &Fixups,
                                           const MCSubtargetInfo &STI) const {
  MCOperand SrcSymbol = MI.getOperand(3);
  assert(SrcSymbol.isExpr() &&
         "Expected expression as first input to TLSDESCCALL");
  const auto *Expr = dyn_cast<MCSpecifierExpr>(SrcSymbol.getExpr());
  MCRegister Link = MI.getOperand(0).getReg();
  MCRegister Dest = MI.getOperand(1).getReg();
  int64_t Imm = MI.getOperand(2).getImm();
  addFixup(Fixups, 0, Expr, ELF::R_RISCV_TLSDESC_CALL);
  MCInst Call =
      MCInstBuilder(RVTrim::JALR).addReg(Link).addReg(Dest).addImm(Imm);

  uint32_t Binary = getBinaryCodeForInstr(Call, Fixups, STI);
  support::endian::write(CB, Binary, llvm::endianness::little);
}

// Expand PseudoAddTPRel to a simple ADD with the correct relocation.
void RVTrimMCCodeEmitter::expandAddTPRel(const MCInst &MI,
                                        SmallVectorImpl<char> &CB,
                                        SmallVectorImpl<MCFixup> &Fixups,
                                        const MCSubtargetInfo &STI) const {
  MCOperand DestReg = MI.getOperand(0);
  MCOperand SrcReg = MI.getOperand(1);
  MCOperand TPReg = MI.getOperand(2);
  assert(TPReg.isReg() && TPReg.getReg() == RVTrim::X4 &&
         "Expected thread pointer as second input to TP-relative add");

  MCOperand SrcSymbol = MI.getOperand(3);
  assert(SrcSymbol.isExpr() &&
         "Expected expression as third input to TP-relative add");

  const auto *Expr = dyn_cast<MCSpecifierExpr>(SrcSymbol.getExpr());
  assert(Expr && Expr->getSpecifier() == ELF::R_RISCV_TPREL_ADD &&
         "Expected tprel_add relocation on TP-relative symbol");

  addFixup(Fixups, 0, Expr, ELF::R_RISCV_TPREL_ADD);
  if (STI.hasFeature(RVTrim::FeatureRelax))
    Fixups.back().setLinkerRelaxable();

  // Emit a normal ADD instruction with the given operands.
  MCInst TmpInst = MCInstBuilder(RVTrim::ADD)
                       .addOperand(DestReg)
                       .addOperand(SrcReg)
                       .addOperand(TPReg);
  uint32_t Binary = getBinaryCodeForInstr(TmpInst, Fixups, STI);
  support::endian::write(CB, Binary, llvm::endianness::little);
}

static unsigned getInvertedBranchOp(unsigned BrOp) {
  switch (BrOp) {
  default:
    llvm_unreachable("Unexpected branch opcode!");
  case RVTrim::PseudoLongBEQ:
    return RVTrim::BNE;
  case RVTrim::PseudoLongBNE:
    return RVTrim::BEQ;
  case RVTrim::PseudoLongBEQI:
    return RVTrim::BNEI;
  case RVTrim::PseudoLongBNEI:
    return RVTrim::BEQI;
  case RVTrim::PseudoLongBLT:
    return RVTrim::BGE;
  case RVTrim::PseudoLongBGE:
    return RVTrim::BLT;
  case RVTrim::PseudoLongBLTU:
    return RVTrim::BGEU;
  case RVTrim::PseudoLongBGEU:
    return RVTrim::BLTU;
  case RVTrim::PseudoLongQC_BEQI:
    return RVTrim::QC_BNEI;
  case RVTrim::PseudoLongQC_BNEI:
    return RVTrim::QC_BEQI;
  case RVTrim::PseudoLongQC_BLTI:
    return RVTrim::QC_BGEI;
  case RVTrim::PseudoLongQC_BGEI:
    return RVTrim::QC_BLTI;
  case RVTrim::PseudoLongQC_BLTUI:
    return RVTrim::QC_BGEUI;
  case RVTrim::PseudoLongQC_BGEUI:
    return RVTrim::QC_BLTUI;
  case RVTrim::PseudoLongQC_E_BEQI:
    return RVTrim::QC_E_BNEI;
  case RVTrim::PseudoLongQC_E_BNEI:
    return RVTrim::QC_E_BEQI;
  case RVTrim::PseudoLongQC_E_BLTI:
    return RVTrim::QC_E_BGEI;
  case RVTrim::PseudoLongQC_E_BGEI:
    return RVTrim::QC_E_BLTI;
  case RVTrim::PseudoLongQC_E_BLTUI:
    return RVTrim::QC_E_BGEUI;
  case RVTrim::PseudoLongQC_E_BGEUI:
    return RVTrim::QC_E_BLTUI;
  case RVTrim::PseudoLongCV_BEQIMM:
    return RVTrim::CV_BNEIMM;
  case RVTrim::PseudoLongCV_BNEIMM:
    return RVTrim::CV_BEQIMM;
  }
}

// Expand PseudoLongBxx to an inverted conditional branch and an unconditional
// jump.
void RVTrimMCCodeEmitter::expandLongCondBr(const MCInst &MI,
                                          SmallVectorImpl<char> &CB,
                                          SmallVectorImpl<MCFixup> &Fixups,
                                          const MCSubtargetInfo &STI) const {
  MCRegister SrcReg1 = MI.getOperand(0).getReg();
  const MCOperand &Src2 = MI.getOperand(1);
  const MCOperand &SrcSymbol = MI.getOperand(2);
  unsigned Opcode = MI.getOpcode();
  bool IsEqTest =
      Opcode == RVTrim::PseudoLongBNE || Opcode == RVTrim::PseudoLongBEQ;

  bool UseCompressedBr = false;
  if (IsEqTest && STI.hasFeature(RVTrim::FeatureStdExtZca)) {
    MCRegister SrcReg2 = Src2.getReg();
    if (RVTrim::X8 <= SrcReg1.id() && SrcReg1.id() <= RVTrim::X15 &&
        SrcReg2.id() == RVTrim::X0) {
      UseCompressedBr = true;
    } else if (RVTrim::X8 <= SrcReg2.id() && SrcReg2.id() <= RVTrim::X15 &&
               SrcReg1.id() == RVTrim::X0) {
      std::swap(SrcReg1, SrcReg2);
      UseCompressedBr = true;
    }
  }

  uint32_t Offset;
  if (UseCompressedBr) {
    unsigned InvOpc =
        Opcode == RVTrim::PseudoLongBNE ? RVTrim::C_BEQZ : RVTrim::C_BNEZ;
    MCInst TmpInst = MCInstBuilder(InvOpc).addReg(SrcReg1).addImm(6);
    uint16_t Binary = getBinaryCodeForInstr(TmpInst, Fixups, STI);
    support::endian::write<uint16_t>(CB, Binary, llvm::endianness::little);
    Offset = 2;
  } else {
    unsigned InvOpc = getInvertedBranchOp(Opcode);
    MCInst TmpInst =
        MCInstBuilder(InvOpc).addReg(SrcReg1).addOperand(Src2).addImm(8);
    uint32_t Binary = getBinaryCodeForInstr(TmpInst, Fixups, STI);
    support::endian::write(CB, Binary, llvm::endianness::little);
    Offset = 4;
  }

  // Save the number fixups.
  size_t FixupStartIndex = Fixups.size();

  // Emit an unconditional jump to the destination.
  MCInst TmpInst =
      MCInstBuilder(RVTrim::JAL).addReg(RVTrim::X0).addOperand(SrcSymbol);
  uint32_t Binary = getBinaryCodeForInstr(TmpInst, Fixups, STI);
  support::endian::write(CB, Binary, llvm::endianness::little);

  // Drop any fixup added so we can add the correct one.
  Fixups.resize(FixupStartIndex);

  if (SrcSymbol.isExpr()) {
    addFixup(Fixups, Offset, SrcSymbol.getExpr(), RVTrim::fixup_rvtrim_jal);
    if (STI.hasFeature(RVTrim::FeatureRelax))
      Fixups.back().setLinkerRelaxable();
  }
}

// Expand PseudoLongQC_(E_)Bxxx to an inverted conditional branch and an
// unconditional jump.
void RVTrimMCCodeEmitter::expandQCLongCondBrImm(const MCInst &MI,
                                               SmallVectorImpl<char> &CB,
                                               SmallVectorImpl<MCFixup> &Fixups,
                                               const MCSubtargetInfo &STI,
                                               unsigned Size) const {
  MCRegister SrcReg1 = MI.getOperand(0).getReg();
  auto BrImm = MI.getOperand(1).getImm();
  MCOperand SrcSymbol = MI.getOperand(2);
  unsigned Opcode = MI.getOpcode();
  uint32_t Offset;
  unsigned InvOpc = getInvertedBranchOp(Opcode);
  // Emit inverted conditional branch with offset:
  // 8 (QC.BXXX(4) + JAL(4))
  // or
  // 10 (QC.E.BXXX(6) + JAL(4)).
  if (Size == 4) {
    MCInst TmpBr =
        MCInstBuilder(InvOpc).addReg(SrcReg1).addImm(BrImm).addImm(8);
    uint32_t BrBinary = getBinaryCodeForInstr(TmpBr, Fixups, STI);
    support::endian::write(CB, BrBinary, llvm::endianness::little);
  } else {
    MCInst TmpBr =
        MCInstBuilder(InvOpc).addReg(SrcReg1).addImm(BrImm).addImm(10);
    uint64_t BrBinary =
        getBinaryCodeForInstr(TmpBr, Fixups, STI) & 0xffff'ffff'ffffu;
    SmallVector<char, 8> Encoding;
    support::endian::write(Encoding, BrBinary, llvm::endianness::little);
    assert(Encoding[6] == 0 && Encoding[7] == 0 &&
           "Unexpected encoding for 48-bit instruction");
    Encoding.truncate(6);
    CB.append(Encoding);
  }
  Offset = Size;
  // Save the number fixups.
  size_t FixupStartIndex = Fixups.size();
  // Emit an unconditional jump to the destination.
  MCInst TmpJ =
      MCInstBuilder(RVTrim::JAL).addReg(RVTrim::X0).addOperand(SrcSymbol);
  uint32_t JBinary = getBinaryCodeForInstr(TmpJ, Fixups, STI);
  support::endian::write(CB, JBinary, llvm::endianness::little);
  // Drop any fixup added so we can add the correct one.
  Fixups.resize(FixupStartIndex);
  if (SrcSymbol.isExpr()) {
    addFixup(Fixups, Offset, SrcSymbol.getExpr(), RVTrim::fixup_rvtrim_jal);
    if (STI.hasFeature(RVTrim::FeatureRelax))
      Fixups.back().setLinkerRelaxable();
  }
}

void RVTrimMCCodeEmitter::expandPseudoQCAccess(
    const MCInst &MI, SmallVectorImpl<char> &CB,
    SmallVectorImpl<MCFixup> &Fixups, const MCSubtargetInfo &STI) const {
  unsigned AccessOpc;

  switch (MI.getOpcode()) {
#define QC_ACCESS_CASE(_Suffix)                                                \
  case RVTrim::PseudoQCAccess##_Suffix:                                         \
    AccessOpc = RVTrim::_Suffix;                                                \
    break;
    // clang-format off
  QC_ACCESS_CASE(LB)
  QC_ACCESS_CASE(LBU)
  QC_ACCESS_CASE(LH)
  QC_ACCESS_CASE(LHU)
  QC_ACCESS_CASE(LW)
  QC_ACCESS_CASE(SB)
  QC_ACCESS_CASE(SH)
  QC_ACCESS_CASE(SW)
  QC_ACCESS_CASE(C_LBU)
  QC_ACCESS_CASE(C_LH)
  QC_ACCESS_CASE(C_LHU)
  QC_ACCESS_CASE(C_LW)
  QC_ACCESS_CASE(C_SB)
  QC_ACCESS_CASE(C_SH)
  QC_ACCESS_CASE(C_SW)
  // clang-format on
  default:
    llvm_unreachable("Unhandled QC Access Opcode");
  };

  MCInst TmpAccess = MCInstBuilder(AccessOpc)
                         .addOperand(MI.getOperand(0))
                         .addOperand(MI.getOperand(1))
                         .addOperand(MI.getOperand(2));
  unsigned Size = MCII.get(AccessOpc).getSize();
  uint16_t FixupKind;
  switch (Size) {
  default:
    llvm_unreachable("Unhandled QC Access Instruction Size");
  case 2: {
    uint16_t AccessBinary = getBinaryCodeForInstr(TmpAccess, Fixups, STI);
    support::endian::write(CB, AccessBinary, llvm::endianness::little);
    FixupKind = RVTrim::fixup_qc_access_16;
    break;
  }
  case 4: {
    uint32_t AccessBinary = getBinaryCodeForInstr(TmpAccess, Fixups, STI);
    support::endian::write(CB, AccessBinary, llvm::endianness::little);
    FixupKind = RVTrim::fixup_qc_access_32;
    break;
  }
  }
  // Only emit the qc.access fixup if linker relaxation is enabled. The pass has
  // already checked for this before using the Pseudos, but the user may have
  // written the instructions directly in assembly.
  if (!STI.hasFeature(RVTrim::FeatureRelax))
    return;

  const MCOperand &AccessSymbol = MI.getOperand(3);
  assert(AccessSymbol.isExpr() && "Expected expression in PseudoQCAccess");

  const auto *AccessExpr = cast<MCSpecifierExpr>(AccessSymbol.getExpr());
  assert(AccessExpr->getSpecifier() == RVTrim::S_QC_ACCESS &&
         "Expected qc.access specifier on symbol");

  addFixup(Fixups, /*Offset=*/0, AccessExpr, FixupKind);
  // The added fixup is always linker relaxable.
  Fixups.back().setLinkerRelaxable();
}

void RVTrimMCCodeEmitter::encodeInstruction(const MCInst &MI,
                                           SmallVectorImpl<char> &CB,
                                           SmallVectorImpl<MCFixup> &Fixups,
                                           const MCSubtargetInfo &STI) const {
  const MCInstrDesc &Desc = MCII.get(MI.getOpcode());
  // Get byte count of instruction.
  unsigned Size = Desc.getSize();

  // RVTrimInstrInfo::getInstSizeInBytes expects that the total size of the
  // expanded instructions for each pseudo is correct in the Size field of the
  // tablegen definition for the pseudo.
  switch (MI.getOpcode()) {
  default:
    break;
  case RVTrim::PseudoCALLReg:
  case RVTrim::PseudoCALL:
  case RVTrim::PseudoTAIL:
  case RVTrim::PseudoTAILReg:
  case RVTrim::PseudoJump:
    expandFunctionCall(MI, CB, Fixups, STI);
    MCNumEmitted += 2;
    return;
  case RVTrim::PseudoCALLLpadAlign:
    expandFunctionCallLpad(MI, CB, Fixups, STI);
    MCNumEmitted += 3; // AUIPC + JALR + LPAD
    return;
  case RVTrim::PseudoCALLIndirectLpadAlign:
    expandFunctionCallLpad(MI, CB, Fixups, STI);
    MCNumEmitted += 2; // JALR + LPAD
    return;
  case RVTrim::PseudoAddTPRel:
    expandAddTPRel(MI, CB, Fixups, STI);
    MCNumEmitted += 1;
    return;
  case RVTrim::PseudoLongBEQ:
  case RVTrim::PseudoLongBNE:
  case RVTrim::PseudoLongBEQI:
  case RVTrim::PseudoLongBNEI:
  case RVTrim::PseudoLongBLT:
  case RVTrim::PseudoLongBGE:
  case RVTrim::PseudoLongBLTU:
  case RVTrim::PseudoLongBGEU:
    expandLongCondBr(MI, CB, Fixups, STI);
    MCNumEmitted += 2;
    return;
  case RVTrim::PseudoLongQC_BEQI:
  case RVTrim::PseudoLongQC_BNEI:
  case RVTrim::PseudoLongQC_BLTI:
  case RVTrim::PseudoLongQC_BGEI:
  case RVTrim::PseudoLongQC_BLTUI:
  case RVTrim::PseudoLongQC_BGEUI:
  case RVTrim::PseudoLongCV_BEQIMM:
  case RVTrim::PseudoLongCV_BNEIMM:
    expandQCLongCondBrImm(MI, CB, Fixups, STI, 4);
    MCNumEmitted += 2;
    return;
  case RVTrim::PseudoLongQC_E_BEQI:
  case RVTrim::PseudoLongQC_E_BNEI:
  case RVTrim::PseudoLongQC_E_BLTI:
  case RVTrim::PseudoLongQC_E_BGEI:
  case RVTrim::PseudoLongQC_E_BLTUI:
  case RVTrim::PseudoLongQC_E_BGEUI:
    expandQCLongCondBrImm(MI, CB, Fixups, STI, 6);
    MCNumEmitted += 2;
    return;
  case RVTrim::PseudoTLSDESCCall:
    expandTLSDESCCall(MI, CB, Fixups, STI);
    MCNumEmitted += 1;
    return;
  case RVTrim::PseudoQCAccessLB:
  case RVTrim::PseudoQCAccessLBU:
  case RVTrim::PseudoQCAccessLH:
  case RVTrim::PseudoQCAccessLHU:
  case RVTrim::PseudoQCAccessLW:
  case RVTrim::PseudoQCAccessSB:
  case RVTrim::PseudoQCAccessSH:
  case RVTrim::PseudoQCAccessSW:
  case RVTrim::PseudoQCAccessC_LBU:
  case RVTrim::PseudoQCAccessC_LH:
  case RVTrim::PseudoQCAccessC_LHU:
  case RVTrim::PseudoQCAccessC_LW:
  case RVTrim::PseudoQCAccessC_SB:
  case RVTrim::PseudoQCAccessC_SH:
  case RVTrim::PseudoQCAccessC_SW:
    expandPseudoQCAccess(MI, CB, Fixups, STI);
    MCNumEmitted += 1;
    return;
  }

  switch (Size) {
  default:
    llvm_unreachable("Unhandled encodeInstruction length!");
  case 2: {
    uint16_t Bits = getBinaryCodeForInstr(MI, Fixups, STI);
    support::endian::write<uint16_t>(CB, Bits, llvm::endianness::little);
    break;
  }
  case 4: {
    uint32_t Bits = getBinaryCodeForInstr(MI, Fixups, STI);
    support::endian::write(CB, Bits, llvm::endianness::little);
    break;
  }
  case 6: {
    uint64_t Bits = getBinaryCodeForInstr(MI, Fixups, STI) & 0xffff'ffff'ffffu;
    SmallVector<char, 8> Encoding;
    support::endian::write(Encoding, Bits, llvm::endianness::little);
    assert(Encoding[6] == 0 && Encoding[7] == 0 &&
           "Unexpected encoding for 48-bit instruction");
    Encoding.truncate(6);
    CB.append(Encoding);
    break;
  }
  case 8: {
    uint64_t Bits = getBinaryCodeForInstr(MI, Fixups, STI);
    support::endian::write(CB, Bits, llvm::endianness::little);
    break;
  }
  }

  ++MCNumEmitted; // Keep track of the # of mi's emitted.
}

uint64_t
RVTrimMCCodeEmitter::getMachineOpValue(const MCInst &MI, const MCOperand &MO,
                                      SmallVectorImpl<MCFixup> &Fixups,
                                      const MCSubtargetInfo &STI) const {

  if (MO.isReg())
    return Ctx.getRegisterInfo()->getEncodingValue(MO.getReg());

  if (MO.isImm())
    return MO.getImm();

  llvm_unreachable("Unhandled expression!");
  return 0;
}

uint64_t
RVTrimMCCodeEmitter::getImmOpValueMinus1(const MCInst &MI, unsigned OpNo,
                                        SmallVectorImpl<MCFixup> &Fixups,
                                        const MCSubtargetInfo &STI) const {
  const MCOperand &MO = MI.getOperand(OpNo);

  if (MO.isImm()) {
    uint64_t Res = MO.getImm();
    return (Res - 1);
  }

  llvm_unreachable("Unhandled expression!");
  return 0;
}

uint64_t
RVTrimMCCodeEmitter::getImmOpValueSlist(const MCInst &MI, unsigned OpNo,
                                       SmallVectorImpl<MCFixup> &Fixups,
                                       const MCSubtargetInfo &STI) const {
  const MCOperand &MO = MI.getOperand(OpNo);
  assert(MO.isImm() && "Slist operand must be immediate");

  uint64_t Res = MO.getImm();
  switch (Res) {
  case 0:
    return 0;
  case 1:
    return 1;
  case 2:
    return 2;
  case 4:
    return 3;
  case 8:
    return 4;
  case 16:
    return 5;
  case 15:
    return 6;
  case 31:
    return 7;
  default:
    llvm_unreachable("Unhandled Slist value!");
  }
}

template <unsigned N>
unsigned
RVTrimMCCodeEmitter::getImmOpValueAsrN(const MCInst &MI, unsigned OpNo,
                                      SmallVectorImpl<MCFixup> &Fixups,
                                      const MCSubtargetInfo &STI) const {
  const MCOperand &MO = MI.getOperand(OpNo);

  if (MO.isImm()) {
    uint64_t Res = MO.getImm();
    assert((Res & ((1 << N) - 1)) == 0 && "LSB is non-zero");
    return Res >> N;
  }

  return getImmOpValue(MI, OpNo, Fixups, STI);
}

uint64_t
RVTrimMCCodeEmitter::getImmOpValueZibi(const MCInst &MI, unsigned OpNo,
                                      SmallVectorImpl<MCFixup> &Fixups,
                                      const MCSubtargetInfo &STI) const {
  const MCOperand &MO = MI.getOperand(OpNo);
  assert(MO.isImm() && "Zibi operand must be an immediate");
  int64_t Res = MO.getImm();
  if (Res == -1)
    return 0;

  return Res;
}

uint64_t RVTrimMCCodeEmitter::getImmOpValue(const MCInst &MI, unsigned OpNo,
                                           SmallVectorImpl<MCFixup> &Fixups,
                                           const MCSubtargetInfo &STI) const {
  bool EnableRelax = STI.hasFeature(RVTrim::FeatureRelax);
  const MCOperand &MO = MI.getOperand(OpNo);

  MCInstrDesc const &Desc = MCII.get(MI.getOpcode());
  unsigned MIFrm = RVTrimII::getFormat(Desc.TSFlags);

  // If the destination is an immediate, there is nothing to do.
  if (MO.isImm())
    return MO.getImm();

  assert(MO.isExpr() &&
         "getImmOpValue expects only expressions or immediates");
  const MCExpr *Expr = MO.getExpr();
  MCExpr::ExprKind Kind = Expr->getKind();

  // `RelaxCandidate` must be set to `true` in two cases:
  // - The fixup's relocation gets a R_RISCV_RELAX relocation
  // - The underlying instruction may be relaxed to an instruction that gets a
  //   `R_RISCV_RELAX` relocation.
  //
  // The actual emission of `R_RISCV_RELAX` will be handled in
  // `RVTrimAsmBackend::applyFixup`.
  bool RelaxCandidate = false;
  auto AsmRelaxToLinkerRelaxable = [&]() -> void {
    if (!STI.hasFeature(RVTrim::FeatureExactAssembly))
      RelaxCandidate = true;
  };

  unsigned FixupKind = RVTrim::fixup_rvtrim_invalid;
  if (Kind == MCExpr::Specifier) {
    const auto *RVExpr = cast<MCSpecifierExpr>(Expr);
    FixupKind = RVExpr->getSpecifier();
    switch (RVExpr->getSpecifier()) {
    default:
      assert(FixupKind && FixupKind < FirstTargetFixupKind &&
             "invalid specifier");
      break;
    case ELF::R_RISCV_TPREL_ADD:
      // tprel_add is only used to indicate that a relocation should be emitted
      // for an add instruction used in TP-relative addressing. It should not be
      // expanded as if representing an actual instruction operand and so to
      // encounter it here is an error.
      llvm_unreachable(
          "ELF::R_RISCV_TPREL_ADD should not represent an instruction operand");
    case RVTrim::S_QC_ACCESS:
      // The same logic for tprel_add applies to S_QC_ACCESS, for similar
      // reasons, but we use a specifier becuase %qc.access() gets expanded
      // differently depending on the underlying instruction.
      llvm_unreachable(
          "S_QC_ACCESS should not represent an instruction operand");
    case RVTrim::S_LO:
      if (MIFrm == RVTrimII::InstFormatI)
        FixupKind = RVTrim::fixup_rvtrim_lo12_i;
      else if (MIFrm == RVTrimII::InstFormatS)
        FixupKind = RVTrim::fixup_rvtrim_lo12_s;
      else
        llvm_unreachable("VK_LO used with unexpected instruction format");
      RelaxCandidate = true;
      break;
    case ELF::R_RISCV_HI20:
      FixupKind = RVTrim::fixup_rvtrim_hi20;
      RelaxCandidate = true;
      break;
    case RVTrim::S_PCREL_LO:
      if (MIFrm == RVTrimII::InstFormatI)
        FixupKind = RVTrim::fixup_rvtrim_pcrel_lo12_i;
      else if (MIFrm == RVTrimII::InstFormatS)
        FixupKind = RVTrim::fixup_rvtrim_pcrel_lo12_s;
      else
        llvm_unreachable("VK_PCREL_LO used with unexpected instruction format");
      RelaxCandidate = true;
      break;
    case RVTrim::S_PCREL_HI:
      FixupKind = RVTrim::fixup_rvtrim_pcrel_hi20;
      RelaxCandidate = true;
      break;
    case RVTrim::S_GOT_HI:
      FixupKind = ELF::R_RISCV_GOT_HI20;
      RelaxCandidate = true;
      break;
    case RVTrim::S_TPREL_LO:
      if (MIFrm == RVTrimII::InstFormatI)
        FixupKind = ELF::R_RISCV_TPREL_LO12_I;
      else if (MIFrm == RVTrimII::InstFormatS)
        FixupKind = ELF::R_RISCV_TPREL_LO12_S;
      else
        llvm_unreachable("VK_TPREL_LO used with unexpected instruction format");
      RelaxCandidate = true;
      break;
    case RVTrim::S_CALL_PLT:
      if (Ctx.getTargetTriple().isOSBinFormatMachO()) {
        FixupKind = RVTrim::fixup_rvtrim_jal;
        break;
      }
      FixupKind = RVTrim::fixup_rvtrim_call_plt;
      RelaxCandidate = true;
      break;
    case RVTrim::S_QC_ABS20:
      FixupKind = RVTrim::fixup_rvtrim_qc_abs20_u;
      RelaxCandidate = true;
      break;
    case ELF::R_RISCV_GOT_HI20:
    case ELF::R_RISCV_TPREL_HI20:
    case ELF::R_RISCV_TLSDESC_HI20:
      RelaxCandidate = true;
      break;
    }
  } else if (Kind == MCExpr::SymbolRef || Kind == MCExpr::Binary) {
    // FIXME: Sub kind binary exprs have chance of underflow.
    if (MIFrm == RVTrimII::InstFormatJ) {
      FixupKind = RVTrim::fixup_rvtrim_jal;
      RelaxCandidate = true;
    } else if (MIFrm == RVTrimII::InstFormatB) {
      FixupKind = RVTrim::fixup_rvtrim_branch;
      // Relaxes to B<cc>; JAL, with fixup_rvtrim_jal
      AsmRelaxToLinkerRelaxable();
    } else if (MIFrm == RVTrimII::InstFormatCJ) {
      FixupKind = RVTrim::fixup_rvtrim_rvc_jump;
      // Relaxes to JAL with fixup_rvtrim_jal
      AsmRelaxToLinkerRelaxable();
    } else if (MIFrm == RVTrimII::InstFormatCB) {
      FixupKind = RVTrim::fixup_rvtrim_rvc_branch;
      // Relaxes to B<cc>; JAL, with fixup_rvtrim_jal
      AsmRelaxToLinkerRelaxable();
    } else if (MIFrm == RVTrimII::InstFormatCI) {
      FixupKind = RVTrim::fixup_rvtrim_rvc_imm;
      // Relaxes to `QC.E.LI` with fixup_rvtrim_qc_e_32
      if (STI.hasFeature(RVTrim::FeatureVendorXqcili))
        AsmRelaxToLinkerRelaxable();
    } else if (MIFrm == RVTrimII::InstFormatI) {
      FixupKind = RVTrim::fixup_rvtrim_12_i;
    } else if (MIFrm == RVTrimII::InstFormatQC_EB) {
      FixupKind = RVTrim::fixup_rvtrim_qc_e_branch;
      // Relaxes to QC.E.B<cc>I; JAL, with fixup_rvtrim_jal
      AsmRelaxToLinkerRelaxable();
    } else if (MIFrm == RVTrimII::InstFormatQC_EAI) {
      FixupKind = RVTrim::fixup_rvtrim_qc_e_32;
      RelaxCandidate = true;
    } else if (MIFrm == RVTrimII::InstFormatQC_EJ) {
      FixupKind = RVTrim::fixup_rvtrim_qc_e_call_plt;
      RelaxCandidate = true;
    } else if (MIFrm == RVTrimII::InstFormatNDS_BRANCH_10) {
      FixupKind = RVTrim::fixup_rvtrim_nds_branch_10;
    }
  }

  assert(FixupKind != RVTrim::fixup_rvtrim_invalid && "Unhandled expression!");

  addFixup(Fixups, 0, Expr, FixupKind);
  // If linker relaxation is enabled and supported by this relocation, set a bit
  // so that the assembler knows the size of the instruction is not fixed/known,
  // and the relocation will need a R_RISCV_RELAX relocation.
  if (EnableRelax && RelaxCandidate)
    Fixups.back().setLinkerRelaxable();
  ++MCNumFixups;

  return 0;
}

unsigned
RVTrimMCCodeEmitter::getYBNDSWImmOpValue(const MCInst &MI, unsigned OpNo,
                                        SmallVectorImpl<MCFixup> &Fixups,
                                        const MCSubtargetInfo &STI) const {
  unsigned Imm = getImmOpValue(MI, OpNo, Fixups, STI);
  assert(RVTrim::isValidYBNDSWImm(Imm) && "Should have been checked before");
  // YBNDSWI decodes to the requested length result as follows:
  // If imm[8:0] == 0, result is 4096.
  if (Imm == 4096)
    return 0;
  // If imm[8] == 0 and imm[7:0] != 0, result is imm[7:0] (1, 2, ..., 255).
  if (Imm > 0 && Imm <= 255)
    return Imm;
  // If imm[8] == 1 and imm[7:5] == 0, result is
  //   `256 | (imm[3:0] << 4) | (imm[4] << 3)` (256, 264, ..., 504).
  if (Imm >= 256 && Imm <= 504 && (Imm % 8) == 0) {
    // Encode the multiples of 8 in this range in odd-even buckets, setting bit
    // 4 of the immediate to 1 for odd multiples of 8.
    unsigned MultipleOf8 = (Imm - 256) >> 3;
    unsigned OddMultiple = MultipleOf8 & 1;
    unsigned Bits3To0 = MultipleOf8 >> 1;
    return 256 | (OddMultiple << 4) | Bits3To0;
  }
  // Otherwise, result is imm[7:0] << 4 (512, 528, ... 4080).
  if (Imm >= 512 && Imm <= 4080 && (Imm % 16) == 0)
    return 256 | (Imm >> 4);
  llvm_unreachable("Invalid immediate for YBNDSWI");
}

unsigned RVTrimMCCodeEmitter::getVMaskReg(const MCInst &MI, unsigned OpNo,
                                         SmallVectorImpl<MCFixup> &Fixups,
                                         const MCSubtargetInfo &STI) const {
  MCOperand MO = MI.getOperand(OpNo);
  assert(MO.isReg() && "Expected a register.");

  switch (MO.getReg().id()) {
  default:
    llvm_unreachable("Invalid mask register.");
  case RVTrim::V0:
    return 0;
  case RVTrim::NoRegister:
    return 1;
  }
}

unsigned RVTrimMCCodeEmitter::getRlistOpValue(const MCInst &MI, unsigned OpNo,
                                             SmallVectorImpl<MCFixup> &Fixups,
                                             const MCSubtargetInfo &STI) const {
  const MCOperand &MO = MI.getOperand(OpNo);
  assert(MO.isImm() && "Rlist operand must be immediate");
  auto Imm = MO.getImm();
  assert(Imm >= 4 && "EABI is currently not implemented");
  return Imm;
}
unsigned
RVTrimMCCodeEmitter::getRlistS0OpValue(const MCInst &MI, unsigned OpNo,
                                      SmallVectorImpl<MCFixup> &Fixups,
                                      const MCSubtargetInfo &STI) const {
  const MCOperand &MO = MI.getOperand(OpNo);
  assert(MO.isImm() && "Rlist operand must be immediate");
  auto Imm = MO.getImm();
  assert(Imm >= 4 && "EABI is currently not implemented");
  assert(Imm != RVTrimZC::RA && "Rlist operand must include s0");
  return Imm;
}

#include "RVTrimGenMCCodeEmitter.inc"
