//===-- GPUMinAsmBackend.cpp - GPUMin assembler backend -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The assembler backend patches instruction words once a fixup is resolved and
// tells the ELF writer what kind of object file to produce.  GM32 words are
// fixed at four bytes, so there is no relaxation and no variable-width layout.
//
// Chapter 00 only knows the 32-bit absolute fixup.  Chapter 02 adds the
// PC-relative kinds that GM32 branches use.
//
//===----------------------------------------------------------------------===//

#include "GPUMinFixupKinds.h"
#include "GPUMinMCTargetDesc.h"
#include "llvm/BinaryFormat/ELF.h"
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
#include "llvm/Support/MathExtras.h"

using namespace llvm;

namespace {

class GPUMinAsmBackend : public MCAsmBackend {
  uint8_t OSABI;

public:
  GPUMinAsmBackend(const MCSubtargetInfo &STI, uint8_t OSABI)
      : MCAsmBackend(llvm::endianness::little), OSABI(OSABI) {}
  ~GPUMinAsmBackend() override = default;

  std::unique_ptr<MCObjectTargetWriter>
  createObjectTargetWriter() const override {
    return createGPUMinELFObjectWriter(OSABI);
  }

  void applyFixup(const MCFragment &F, const MCFixup &Fixup,
                  const MCValue &Target, uint8_t *Data, uint64_t Value,
                  bool IsResolved) override;

  /// Padding between functions and alignment gaps.  GM32 has no architecturally
  /// defined NOP yet (chapter 02 adds one), so padding is a zero word, which
  /// the decoder reports as an illegal instruction rather than as EXIT.
  bool writeNopData(raw_ostream &OS, uint64_t Count,
                    const MCSubtargetInfo *STI) const override {
    if (Count % 4)
      return false;
    for (uint64_t I = 0; I != Count; I += 4)
      OS.write_zeros(4);
    return true;
  }

  MCFixupKindInfo getFixupKindInfo(MCFixupKind Kind) const override {
    // clang-format off
    const static MCFixupKindInfo Infos[gpumin::NumTargetFixupKinds] = {
      // name                  offset bits flags
      {"fixup_gpumin_32",          0, 32, 0},
      {"fixup_gpumin_16_pcrel",    0, 16, 0},
    };
    // clang-format on
    static_assert(std::size(Infos) == gpumin::NumTargetFixupKinds,
                  "Not all fixup kinds added to Infos array");

    if (Kind < FirstTargetFixupKind)
      return MCAsmBackend::getFixupKindInfo(Kind);

    return Infos[Kind - FirstTargetFixupKind];
  }
};

} // end anonymous namespace

void GPUMinAsmBackend::applyFixup(const MCFragment &F, const MCFixup &Fixup,
                                  const MCValue &Target, uint8_t *Data,
                                  uint64_t Value, bool IsResolved) {
  // Record a relocation first: this also covers generic fixups such as
  // `.word symbol` whose value is not known at assembly time.
  maybeAddReloc(F, Fixup, Target, Value, IsResolved);

  if (Fixup.getKind() < FirstTargetFixupKind) {
    MCFixupKindInfo Info = getFixupKindInfo(Fixup.getKind());
    Value <<= Info.TargetOffset;
    unsigned NumBytes = alignTo(Info.TargetSize + Info.TargetOffset, 8) / 8;
    for (unsigned I = 0; I != NumBytes; ++I)
      Data[I] |= static_cast<uint8_t>((Value >> (8 * I)) & 0xff);
    return;
  }

  if (Fixup.getKind() == gpumin::fixup_gpumin_32) {
    for (unsigned I = 0; I != 4; ++I)
      Data[I] = static_cast<uint8_t>(Value >> (8 * I));
    return;
  }

  assert(Fixup.getKind() == gpumin::fixup_gpumin_16_pcrel &&
         "unknown GPUMin fixup kind");

  // `Value` is already (target - instruction address): the fixup is created
  // with IsPCRel and offset 0, so the assembler subtracted the word's own
  // address.  Convert bytes to words and patch Inst[15:0].
  int64_t Off = static_cast<int64_t>(Value);
  if (Off & 3)
    getContext().reportError(Fixup.getLoc(),
                             "GPUMin: branch target is not 4-byte aligned");
  Off /= 4;
  if (!isInt<16>(Off))
    getContext().reportError(Fixup.getLoc(),
                             "GPUMin: branch target out of range");
  Data[0] = static_cast<uint8_t>(Off & 0xff);
  Data[1] = static_cast<uint8_t>((Off >> 8) & 0xff);
}

MCAsmBackend *llvm::createGPUMinMCAsmBackend(const Target &T,
                                             const MCSubtargetInfo &STI,
                                             const MCRegisterInfo &MRI,
                                             const MCTargetOptions &Options) {
  return new GPUMinAsmBackend(STI, ELF::ELFOSABI_NONE);
}
