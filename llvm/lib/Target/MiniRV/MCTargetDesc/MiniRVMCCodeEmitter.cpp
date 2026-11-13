//===-- MiniRVMCCodeEmitter.cpp - Convert MiniRV code to machine code -----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "MiniRVMCTargetDesc.h"
#include "llvm/MC/MCCodeEmitter.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCFixup.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/Support/EndianStream.h"

using namespace llvm;

namespace {
class MiniRVMCCodeEmitter : public MCCodeEmitter {
  MCContext &Ctx;
  MCInstrInfo const &MCII;

public:
  MiniRVMCCodeEmitter(MCContext &Ctx, MCInstrInfo const &MCII)
      : Ctx(Ctx), MCII(MCII) {}

  void encodeInstruction(const MCInst &MI, SmallVectorImpl<char> &CB,
                         SmallVectorImpl<MCFixup> &Fixups,
                         const MCSubtargetInfo &STI) const override;

  unsigned getMachineOpValue(const MCInst &MI, const MCOperand &MO,
                             SmallVectorImpl<MCFixup> &Fixups,
                             const MCSubtargetInfo &STI) const;

  unsigned getBranchTargetOpValue(const MCInst &MI, unsigned OpNo,
                                  SmallVectorImpl<MCFixup> &Fixups,
                                  const MCSubtargetInfo &STI) const;

  unsigned getJalTargetOpValue(const MCInst &MI, unsigned OpNo,
                               SmallVectorImpl<MCFixup> &Fixups,
                               const MCSubtargetInfo &STI) const;

  uint64_t getBinaryCodeForInstr(const MCInst &MI,
                                 SmallVectorImpl<MCFixup> &Fixups,
                                 const MCSubtargetInfo &STI) const;
};
} // namespace

void MiniRVMCCodeEmitter::encodeInstruction(const MCInst &MI,
                                            SmallVectorImpl<char> &CB,
                                            SmallVectorImpl<MCFixup> &Fixups,
                                            const MCSubtargetInfo &STI) const {
  uint64_t Bits = getBinaryCodeForInstr(MI, Fixups, STI);
  size_t N = CB.size();
  CB.resize(N + 4);
  support::endian::write32le(&CB[N], Bits);
}

unsigned MiniRVMCCodeEmitter::getMachineOpValue(
    const MCInst &MI, const MCOperand &MO, SmallVectorImpl<MCFixup> &Fixups,
    const MCSubtargetInfo &STI) const {
  if (MO.isReg())
    return Ctx.getRegisterInfo()->getEncodingValue(MO.getReg());
  if (MO.isImm())
    return static_cast<unsigned>(MO.getImm());
  llvm_unreachable("MiniRV has no symbol operands");
}

/// A branch immediate is the byte offset divided by two, packed into the
/// twelve bits the format has room for.  A target that is not known yet is
/// left to the assembler through a fixup.
unsigned MiniRVMCCodeEmitter::getBranchTargetOpValue(
    const MCInst &MI, unsigned OpNo, SmallVectorImpl<MCFixup> &Fixups,
    const MCSubtargetInfo &STI) const {
  const MCOperand &MO = MI.getOperand(OpNo);
  if (MO.isImm())
    return static_cast<unsigned>(MO.getImm() >> 1) & 0xfff;

  assert(MO.isExpr() && "Unexpected branch operand");
  Fixups.push_back(MCFixup::create(0, MO.getExpr(),
                                   MCFixupKind(MiniRV::fixup_minirv_branch),
                                   /*PCRel=*/true));
  return 0;
}

unsigned MiniRVMCCodeEmitter::getJalTargetOpValue(
    const MCInst &MI, unsigned OpNo, SmallVectorImpl<MCFixup> &Fixups,
    const MCSubtargetInfo &STI) const {
  const MCOperand &MO = MI.getOperand(OpNo);
  if (MO.isImm())
    return static_cast<unsigned>(MO.getImm() >> 1) & 0xfffff;

  assert(MO.isExpr() && "Unexpected jump operand");
  Fixups.push_back(MCFixup::create(0, MO.getExpr(),
                                   MCFixupKind(MiniRV::fixup_minirv_jal),
                                   /*PCRel=*/true));
  return 0;
}

MCCodeEmitter *llvm::createMiniRVMCCodeEmitter(const MCInstrInfo &MCII,
                                               MCContext &Ctx) {
  return new MiniRVMCCodeEmitter(Ctx, MCII);
}

#include "MiniRVGenMCCodeEmitter.inc"
