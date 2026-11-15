//===-- RVMinAsmBackend.cpp - RVMin assembler backend ---------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The assembler backend is responsible for two things in RVMin:
//
//   1. patching instruction words once a fixup is resolved (branch/jump
//      targets), and
//   2. telling the ELF writer what kind of object file to produce.
//
// RV32I instruction words are fixed at four bytes, so no relaxation is needed
// and `relaxInstruction` stays at its default.
//
//===----------------------------------------------------------------------===//

#include "RVMinFixupKinds.h"
#include "llvm/BinaryFormat/ELF.h"
#include "RVMinMCTargetDesc.h"
#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCAssembler.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCELFObjectWriter.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCFixup.h"
#include "llvm/MC/MCObjectWriter.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/MCTargetOptions.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace {

/// Write a B-type immediate into an existing instruction word.
/// Layout: imm[12|10:5] | ... | imm[4:1|11] | opcode.
static uint32_t encodeBranchImm(uint32_t Inst, uint32_t Imm) {
  Inst &= ~0xfe000f80u;                 // clear imm[12|10:5] and imm[4:1|11]
  Inst |= (Imm & 0x1000u) << 19;        // imm[12]  -> bit 31
  Inst |= (Imm & 0x07e0u) << 20;        // imm[10:5] -> bits 30:25
  Inst |= (Imm & 0x001eu) << 7;         // imm[4:1] -> bits 11:8
  Inst |= (Imm & 0x0800u) >> 4;         // imm[11]  -> bit 7
  return Inst;
}

/// Write a J-type immediate into an existing instruction word.
/// Layout: imm[20|10:1|11|19:12] | rd | opcode.
static uint32_t encodeJalImm(uint32_t Inst, uint32_t Imm) {
  Inst &= ~0xfffff000u;                 // clear imm[20|10:1|11|19:12]
  Inst |= (Imm & 0x100000u) << 11;      // imm[20]    -> bit 31
  Inst |= (Imm & 0x0007feu) << 20;      // imm[10:1]  -> bits 30:21
  Inst |= (Imm & 0x000800u) << 9;       // imm[11]    -> bit 20
  Inst |= (Imm & 0x0ff000u);            // imm[19:12] -> bits 19:12
  return Inst;
}

class RVMinAsmBackend : public MCAsmBackend {
  uint8_t OSABI;

public:
  RVMinAsmBackend(const MCSubtargetInfo &STI, uint8_t OSABI)
      : MCAsmBackend(llvm::endianness::little), OSABI(OSABI) {}
  ~RVMinAsmBackend() override = default;

  std::unique_ptr<MCObjectTargetWriter>
  createObjectTargetWriter() const override {
    return createRVMinELFObjectWriter(OSABI);
  }

  void applyFixup(const MCFragment &F, const MCFixup &Fixup,
                  const MCValue &Target, uint8_t *Data, uint64_t Value,
                  bool IsResolved) override;

  MCFixupKindInfo getFixupKindInfo(MCFixupKind Kind) const override {
    // clang-format off
    const static MCFixupKindInfo Infos[rvmin::NumTargetFixupKinds] = {
      // name                  offset bits flags
      {"fixup_rvmin_32",         0,  32, 0},
      {"fixup_rvmin_branch",     0,  13, 0},
      {"fixup_rvmin_jal",        0,  21, 0},
    };
    // clang-format on
    static_assert(std::size(Infos) == rvmin::NumTargetFixupKinds,
                  "Not all fixup kinds added to Infos array");

    if (Kind < FirstTargetFixupKind)
      return MCAsmBackend::getFixupKindInfo(Kind);

    return Infos[Kind - FirstTargetFixupKind];
  }

  bool writeNopData(raw_ostream &OS, uint64_t Count,
                    const MCSubtargetInfo *STI) const override;
};

} // end anonymous namespace

void RVMinAsmBackend::applyFixup(const MCFragment &F, const MCFixup &Fixup,
                                 const MCValue &Target, uint8_t *Data,
                                 uint64_t Value, bool IsResolved) {
  // Record a relocation first: this also covers generic fixups such as
  // `.word symbol` whose value is not known at assembly time.
  maybeAddReloc(F, Fixup, Target, Value, IsResolved);

  if (Fixup.getKind() < FirstTargetFixupKind) {
    // Generic fixups (.word, .half, ...): the base class only describes them,
    // so write the value here.
    MCFixupKindInfo Info = getFixupKindInfo(Fixup.getKind());
    Value <<= Info.TargetOffset;
    unsigned NumBytes = alignTo(Info.TargetSize + Info.TargetOffset, 8) / 8;
    for (unsigned I = 0; I != NumBytes; ++I)
      Data[I] |= static_cast<uint8_t>((Value >> (8 * I)) & 0xff);
    return;
  }

  switch (Fixup.getKind()) {
  case rvmin::fixup_rvmin_32:
    // Absolute 32-bit patch.  Only used as a placeholder until chapter 7
    // introduces the hi20/lo12 relocations.
    for (unsigned I = 0; I != 4; ++I)
      Data[I] = static_cast<uint8_t>(Value >> (8 * I));
    return;

  case rvmin::fixup_rvmin_branch: {
    int64_t Offset = static_cast<int32_t>(Value);
    if (Offset & 1)
      getContext().reportError(Fixup.getLoc(),
                               "branch target must be 2-byte aligned");
    if (!isInt<13>(Offset))
      getContext().reportError(Fixup.getLoc(), "branch target out of range");
    uint32_t Inst = support::endian::read32le(Data);
    support::endian::write32le(Data, encodeBranchImm(Inst, Offset));
    return;
  }

  case rvmin::fixup_rvmin_jal: {
    int64_t Offset = static_cast<int32_t>(Value);
    if (Offset & 1)
      getContext().reportError(Fixup.getLoc(), "jump target must be 2-byte aligned");
    if (!isInt<21>(Offset))
      getContext().reportError(Fixup.getLoc(), "jump target out of range");
    uint32_t Inst = support::endian::read32le(Data);
    support::endian::write32le(Data, encodeJalImm(Inst, Offset));
    return;
  }

  default:
    llvm_unreachable("unknown RVMin fixup kind");
  }
}

bool RVMinAsmBackend::writeNopData(raw_ostream &OS, uint64_t Count,
                                   const MCSubtargetInfo *STI) const {
  if (Count % 4)
    return false;

  // The canonical RV32I NOP is `addi zero, zero, 0` (0x00000013).
  for (uint64_t I = 0; I != Count; I += 4)
    OS.write("\x13\x00\x00\x00", 4);
  return true;
}

MCAsmBackend *llvm::createRVMinMCAsmBackend(const Target &T,
                                            const MCSubtargetInfo &STI,
                                            const MCRegisterInfo &MRI,
                                            const MCTargetOptions &Options) {
  return new RVMinAsmBackend(STI, ELF::ELFOSABI_NONE);
}
