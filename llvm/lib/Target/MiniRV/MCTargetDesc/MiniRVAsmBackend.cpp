//===-- MiniRVAsmBackend.cpp - MiniRV assembler backend -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "MiniRVAsmBackend.h"
#include "MiniRVMCTargetDesc.h"
#include "llvm/MC/MCAssembler.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCELFObjectWriter.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCValue.h"
#include "llvm/MC/MCObjectWriter.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/MCSymbolELF.h"
#include "llvm/Support/Endian.h"

using namespace llvm;

std::unique_ptr<MCObjectTargetWriter>
MiniRVAsmBackend::createObjectTargetWriter() const {
  return createMiniRVELFObjectWriter(OSABI);
}

MCFixupKindInfo MiniRVAsmBackend::getFixupKindInfo(MCFixupKind Kind) const {
  static const MCFixupKindInfo Infos[] = {
      // The bit positions inside the instruction are not contiguous, so
      // applyFixup() does the placement itself; these entries only describe
      // the instruction the fixup lives in.
      {"fixup_minirv_branch", 0, 32, 0},
      {"fixup_minirv_jal", 0, 32, 0},
      {"fixup_minirv_pcrel_hi20", 0, 32, 0},
      {"fixup_minirv_pcrel_lo12_i", 0, 32, 0},
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
  // A symbol difference (A - B) cannot be represented by a single relocation:
  // the linker needs an ADD for A and a SUB for B.  Profile data describes a
  // counter relative to its descriptor this way.
  if (Target.getSubSym()) {
    assert(Target.getSpecifier() == 0 &&
           "relocatable SymA-SymB cannot have a relocation specifier");
    unsigned AddType = 0, SubType = 0;
    switch (Fixup.getKind()) {
    case FK_Data_1:
      AddType = ELF::R_RISCV_ADD8;
      SubType = ELF::R_RISCV_SUB8;
      break;
    case FK_Data_2:
      AddType = ELF::R_RISCV_ADD16;
      SubType = ELF::R_RISCV_SUB16;
      break;
    case FK_Data_4:
      AddType = ELF::R_RISCV_ADD32;
      SubType = ELF::R_RISCV_SUB32;
      break;
    case FK_Data_8:
      AddType = ELF::R_RISCV_ADD64;
      SubType = ELF::R_RISCV_SUB64;
      break;
    default:
      llvm_unreachable("unsupported fixup size");
    }
    uint64_t AddValue = 0, SubValue = 0;
    MCValue AddTarget =
        MCValue::get(Target.getAddSym(), nullptr, Target.getConstant());
    MCValue SubTarget = MCValue::get(Target.getSubSym());
    Asm->getWriter().recordRelocation(
        F, MCFixup::create(Fixup.getOffset(), nullptr, AddType), AddTarget,
        AddValue);
    Asm->getWriter().recordRelocation(
        F, MCFixup::create(Fixup.getOffset(), nullptr, SubType), SubTarget,
        SubValue);
    Value = AddValue - SubValue;
    // The two relocations carry the symbols; only the constant part is left
    // for the data bytes, so do not record the difference a third time.
    IsResolved = true;
    // Fall through to write the constant part of the difference.
  }

  // Data fixups -- the debug frame uses a four-byte one -- are not
  // instruction-shaped: write the value as a plain little-endian integer.
  if (Fixup.getKind() < FirstTargetFixupKind) {
    // A data fixup that refers to another section becomes a relocation; the
    // linker fills in the value.
    if (!IsResolved) {
      maybeAddReloc(F, Fixup, Target, Value, IsResolved);
      return;
    }
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
  if (!IsResolved) {
    maybeAddReloc(F, Fixup, Target, Value, IsResolved);
    return;
  }

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
  case MiniRV::fixup_minirv_pcrel_hi20: {
    // The high 20 bits of a PC-relative address, rounded away from zero so
    // that the low part always fits in the signed twelve bits of addi.
    uint32_t Hi = static_cast<uint32_t>((Value + 0x800) >> 12) & 0xfffff;
    Insn &= ~0xfffff000u;
    Insn |= Hi << 12;
    break;
  }
  case MiniRV::fixup_minirv_pcrel_lo12_i: {
    // The low twelve bits of the same address, in the I-format immediate.
    Insn &= ~0xfff00000u;
    Insn |= (static_cast<uint32_t>(Value) & 0xfff) << 20;
    break;
  }
  default:
    llvm_unreachable("Unknown fixup kind!");
  }
  support::endian::write32le(Data, Insn);
}

/// Find the pcrel_hi fixup that a pcrel_lo operand refers to.  The low part
/// names the label of the auipc instruction, so the high fixup is the one at
/// that label's offset in the same fragment.
static const MCFixup *getPCRelHiFixup(const MCSymbol *Sym,
                                      const MCFragment **DFOut) {
  const MCFragment *DF = Sym->getFragment();
  if (!DF)
    return nullptr;

  uint64_t Offset = Sym->getOffset();
  if (DF->getContents().size() == Offset) {
    DF = DF->getNext();
    if (!DF)
      return nullptr;
    Offset = 0;
  }

  for (const MCFixup &F : DF->getFixups()) {
    if (F.getOffset() != Offset)
      continue;
    if (F.getKind() == MiniRV::fixup_minirv_pcrel_hi20) {
      *DFOut = DF;
      return &F;
    }
    break;
  }
  return nullptr;
}

std::optional<bool> MiniRVAsmBackend::evaluateFixup(const MCFragment &,
                                                    MCFixup &Fixup,
                                                    MCValue &Target,
                                                    uint64_t &Value) {
  if (Fixup.getKind() != MiniRV::fixup_minirv_pcrel_lo12_i)
    return {};

  const MCSymbol *AUIPCSym = Target.getAddSym();
  if (!AUIPCSym)
    return true;

  const MCFragment *AUIPCDF = nullptr;
  const MCFixup *AUIPCFixup = getPCRelHiFixup(AUIPCSym, &AUIPCDF);
  if (!AUIPCFixup) {
    getContext().reportError(Fixup.getLoc(),
                             "could not find corresponding pcrel_hi");
    return true;
  }

  MCValue AUIPCTarget;
  if (!AUIPCFixup->getValue()->evaluateAsRelocatable(AUIPCTarget, Asm))
    return true;

  const MCSymbol *TargetSym = AUIPCTarget.getAddSym();
  if (!TargetSym)
    return false;

  const auto &SA = static_cast<const MCSymbolELF &>(*TargetSym);
  if (SA.isUndefined())
    return false;

  bool IsResolved = &SA.getSection() == AUIPCDF->getParent() &&
                    SA.getBinding() == ELF::STB_LOCAL;
  if (!IsResolved)
    return false;

  // The value of both parts is the address relative to the auipc.
  Value = Asm->getSymbolOffset(SA) + AUIPCTarget.getConstant();
  Value -= Asm->getFragmentOffset(*AUIPCDF) + AUIPCFixup->getOffset();
  return true;
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
