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

/// Write a CB-format branch offset into an existing 16-bit instruction.
/// Layout: imm[8|4:3] | rs1' | imm[7:6|2:1|5] | op.
static uint16_t encodeCBImm(uint16_t Inst, int32_t Imm) {
  Inst &= ~0x1c7cu;                    // clear bits 12, 11:10, 6:5, 4:3, 2
  Inst |= ((Imm >> 8) & 0x1) << 12;    // imm[8]   -> bit 12
  Inst |= ((Imm >> 3) & 0x3) << 10;    // imm[4:3] -> bits 11:10
  Inst |= ((Imm >> 6) & 0x3) << 5;     // imm[7:6] -> bits 6:5
  Inst |= ((Imm >> 1) & 0x3) << 3;     // imm[2:1] -> bits 4:3
  Inst |= ((Imm >> 5) & 0x1) << 2;     // imm[5]   -> bit 2
  return Inst;
}

/// Write a CJ-format jump offset into an existing 16-bit instruction.
/// Layout: imm[11|4|9:8|10|6|7|3:1|5] | op.
static uint16_t encodeCJImm(uint16_t Inst, int32_t Imm) {
  Inst &= ~0x1ffcu;                    // clear bits 12:2
  Inst |= ((Imm >> 11) & 0x1) << 12;   // imm[11]  -> bit 12
  Inst |= ((Imm >> 4) & 0x1) << 11;    // imm[4]   -> bit 11
  Inst |= ((Imm >> 8) & 0x3) << 9;     // imm[9:8] -> bits 10:9
  Inst |= ((Imm >> 10) & 0x1) << 8;    // imm[10]  -> bit 8
  Inst |= ((Imm >> 6) & 0x1) << 7;     // imm[6]   -> bit 7
  Inst |= ((Imm >> 7) & 0x1) << 6;     // imm[7]   -> bit 6
  Inst |= ((Imm >> 1) & 0x7) << 3;     // imm[3:1] -> bits 5:3
  Inst |= ((Imm >> 5) & 0x1) << 2;     // imm[5]   -> bit 2
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
      {"fixup_rvmin_hi20",      12,  20, 0},
      {"fixup_rvmin_lo12_i",    20,  12, 0},
      {"fixup_rvmin_lo12_s",     0,  32, 0},
      {"fixup_rvmin_rvc_branch", 0,   9, 0},
      {"fixup_rvmin_rvc_jump",   0,  12, 0},
      {"fixup_rvmin_rvc_imm",    0,  16, 0},
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

  case rvmin::fixup_rvmin_hi20: {
    // lui rd, %hi(sym): bits 31:12 hold (value + 0x800) >> 12.  The rounding
    // matches what %lo(sym) subtracts, so the pair covers the full 32 bits.
    uint32_t Inst = support::endian::read32le(Data);
    uint32_t Hi = static_cast<uint32_t>((Value + 0x800) >> 12) & 0xfffff;
    support::endian::write32le(Data, Inst | (Hi << 12));
    return;
  }

  case rvmin::fixup_rvmin_lo12_i: {
    uint32_t Inst = support::endian::read32le(Data);
    support::endian::write32le(Data,
                               Inst | ((static_cast<uint32_t>(Value) & 0xfff)
                                       << 20));
    return;
  }

  case rvmin::fixup_rvmin_lo12_s: {
    uint32_t V = static_cast<uint32_t>(Value);
    uint32_t Inst = support::endian::read32le(Data);
    Inst |= (V & 0xfe0u) << 20; // imm[11:5] -> bits 31:25
    Inst |= (V & 0x01fu) << 7;  // imm[4:0]  -> bits 11:7
    support::endian::write32le(Data, Inst);
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

  case rvmin::fixup_rvmin_rvc_branch: {
    int64_t Offset = static_cast<int32_t>(Value);
    if (Offset & 1)
      getContext().reportError(Fixup.getLoc(),
                               "compressed branch target must be 2-byte aligned");
    if (!isInt<9>(Offset))
      getContext().reportError(Fixup.getLoc(),
                               "compressed branch target out of range");
    uint16_t Inst = support::endian::read16le(Data);
    support::endian::write16le(Data, encodeCBImm(Inst, Offset));
    return;
  }

  case rvmin::fixup_rvmin_rvc_jump: {
    int64_t Offset = static_cast<int32_t>(Value);
    if (Offset & 1)
      getContext().reportError(Fixup.getLoc(),
                               "compressed jump target must be 2-byte aligned");
    if (!isInt<12>(Offset))
      getContext().reportError(Fixup.getLoc(),
                               "compressed jump target out of range");
    uint16_t Inst = support::endian::read16le(Data);
    support::endian::write16le(Data, encodeCJImm(Inst, Offset));
    return;
  }

  case rvmin::fixup_rvmin_rvc_imm:
    getContext().reportError(
        Fixup.getLoc(),
        "compressed instructions do not support symbolic immediates");
    return;

  default:
    llvm_unreachable("unknown RVMin fixup kind");
  }
}

bool RVMinAsmBackend::writeNopData(raw_ostream &OS, uint64_t Count,
                                   const MCSubtargetInfo *STI) const {
  // The canonical RV32I NOP is `addi zero, zero, 0` (0x00000013).  With C
  // enabled a 2-byte tail is padded with `c.nop` (0x0001) instead of failing
  // alignment; without C every section must stay 4-byte aligned.
  if (Count % 4 == 0) {
    for (uint64_t I = 0; I != Count; I += 4)
      OS.write("\x13\x00\x00\x00", 4);
    return true;
  }

  if (Count % 2 == 0 && STI && STI->hasFeature(RVMin::FeatureStdExtC)) {
    uint64_t I = 0;
    for (; I + 4 <= Count; I += 4)
      OS.write("\x13\x00\x00\x00", 4);
    if (I != Count)
      OS.write("\x01\x00", 2);
    return true;
  }

  return false;
}

MCAsmBackend *llvm::createRVMinMCAsmBackend(const Target &T,
                                            const MCSubtargetInfo &STI,
                                            const MCRegisterInfo &MRI,
                                            const MCTargetOptions &Options) {
  return new RVMinAsmBackend(STI, ELF::ELFOSABI_NONE);
}
