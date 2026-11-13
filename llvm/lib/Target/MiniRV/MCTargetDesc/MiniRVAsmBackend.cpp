//===-- MiniRVAsmBackend.cpp - MiniRV assembler backend -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "MiniRVAsmBackend.h"
#include "MiniRVMCTargetDesc.h"
#include "llvm/MC/MCELFObjectWriter.h"
#include "llvm/MC/MCObjectWriter.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/Support/Endian.h"

using namespace llvm;

std::unique_ptr<MCObjectTargetWriter>
MiniRVAsmBackend::createObjectTargetWriter() const {
  return createMiniRVELFObjectWriter(OSABI);
}

MCFixupKindInfo
MiniRVAsmBackend::getFixupKindInfo(MCFixupKind Kind) const {
  static const MCFixupKindInfo Infos[] = {
      // The bit positions inside the instruction are not contiguous, so
      // applyFixup() does the placement itself; these entries only describe
      // the instruction the fixup lives in.
      {"fixup_minirv_branch", 0, 32, 0},
      {"fixup_minirv_jal", 0, 32, 0},
  };

  if (Kind < FirstTargetFixupKind)
    return MCAsmBackend::getFixupKindInfo(Kind);

  assert(unsigned(Kind - FirstTargetFixupKind) < std::size(Infos) &&
         "Invalid kind!");
  return Infos[Kind - FirstTargetFixupKind];
}

void MiniRVAsmBackend::applyFixup(const MCFragment &F, const MCFixup &Fixup,
                                  const MCValue &Target, uint8_t *Data,
                                  uint64_t Value, bool IsResolved) {
  // Data fixups -- the debug frame uses a four-byte one -- are not
  // instruction-shaped: write the value as a plain little-endian integer.
  if (Fixup.getKind() < FirstTargetFixupKind) {
    if (!IsResolved)
      return;
    switch (getFixupKindInfo(Fixup.getKind()).TargetSize) {
    case 64:
      support::endian::write64le(Data, Value);
      break;
    case 32:
      support::endian::write32le(Data, Value);
      break;
    case 16:
      support::endian::write16le(Data, Value);
      break;
    case 8:
      *Data = static_cast<uint8_t>(Value);
      break;
    default:
      llvm_unreachable("Unsupported data fixup size");
    }
    return;
  }

  // An unresolved fixup becomes a relocation; the linker reads the addend from
  // the RELA entry, so the instruction keeps whatever the encoder wrote.
  if (!IsResolved)
    return;

  uint32_t Insn = support::endian::read32le(Data);
  switch (Fixup.getKind()) {
  case MiniRV::fixup_minirv_branch: {
    // imm12[11] -> 31, imm12[9:4] -> 30-25, imm12[3:0] -> 11-8,
    // imm12[10] -> 7.
    uint32_t Imm = static_cast<uint32_t>(Value >> 1) & 0xfff;
    Insn &= ~0xfe000f80u;
    Insn |= (Imm & 0x800) << 20;
    Insn |= (Imm & 0x3f0) << 21;
    Insn |= (Imm & 0x00f) << 8;
    Insn |= (Imm & 0x400) >> 3;
    break;
  }
  case MiniRV::fixup_minirv_jal: {
    // imm20[19] -> 31, imm20[9:0] -> 30-21, imm20[10] -> 20,
    // imm20[18:11] -> 19-12.
    uint32_t Imm = static_cast<uint32_t>(Value >> 1) & 0xfffff;
    Insn &= ~0xfffff000u;
    Insn |= (Imm & 0x80000) << 12;
    Insn |= (Imm & 0x003ff) << 21;
    Insn |= (Imm & 0x00400) << 10;
    Insn |= (Imm & 0x7f800) << 1;
    break;
  }
  default:
    llvm_unreachable("Unknown fixup kind!");
  }
  support::endian::write32le(Data, Insn);
}

bool MiniRVAsmBackend::writeNopData(raw_ostream &OS, uint64_t Count,
                                    const MCSubtargetInfo *STI) const {
  if (Count % 4)
    return false;
  for (uint64_t I = 0; I != Count; I += 4)
    OS.write("\x13\x00\x00\x00", 4); // addi x0, x0, 0
  return true;
}

MCAsmBackend *llvm::createMiniRVAsmBackend(const Target &T,
                                           const MCSubtargetInfo &STI,
                                           const MCRegisterInfo &MRI,
                                           const MCTargetOptions &Options) {
  return new MiniRVAsmBackend(ELF::ELFOSABI_NONE);
}
