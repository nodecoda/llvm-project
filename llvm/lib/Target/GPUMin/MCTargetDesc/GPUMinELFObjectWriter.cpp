//===-- GPUMinELFObjectWriter.cpp - GPUMin ELF writer ---------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// GM32 objects are 32-bit little-endian ELF relocatables.
//
// Chapter 00 borrowed EM_RISCV so that objdump could open the file at all.
// Chapter 03 gives GM32 its own machine number (EM_GPUMIN, an unofficial
// value above the assigned range) and maps it to Triple::gpumin in
// ELFObjectFile.h.  Without that mapping objdump would still open the file as
// RISC-V and decode every GM32 word as garbage.
//
//===----------------------------------------------------------------------===//

#include "GPUMinFixupKinds.h"
#include "GPUMinMCTargetDesc.h"
#include "llvm/ADT/Twine.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/MC/MCELFObjectWriter.h"
#include "llvm/MC/MCFixup.h"
#include "llvm/MC/MCObjectWriter.h"
#include "llvm/MC/MCValue.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

namespace {

class GPUMinELFObjectWriter : public MCELFObjectTargetWriter {
public:
  explicit GPUMinELFObjectWriter(uint8_t OSABI)
      : MCELFObjectTargetWriter(/*Is64Bit=*/false, OSABI, ELF::EM_GPUMIN,
                                /*HasRelocationAddend=*/true) {}
  ~GPUMinELFObjectWriter() override = default;

protected:
  unsigned getRelocType(const MCFixup &Fixup, const MCValue &Target,
                        bool IsPCRel) const override {
    switch (Fixup.getKind()) {
    case FK_Data_4:
      return IsPCRel ? ELF::R_GPUMIN_32_PCREL : ELF::R_GPUMIN_32;
    case gpumin::fixup_gpumin_32:
      return ELF::R_GPUMIN_32;
    case gpumin::fixup_gpumin_16_pcrel:
      return ELF::R_GPUMIN_16_PCREL;
    case gpumin::fixup_gpumin_21_pcrel:
      return ELF::R_GPUMIN_21_PCREL;
    default:
      reportError(Fixup.getLoc(), Twine("GPUMin: unsupported relocation"));
      return ELF::R_GPUMIN_NONE;
    }
  }
};

} // end anonymous namespace

std::unique_ptr<MCObjectTargetWriter>
llvm::createGPUMinELFObjectWriter(uint8_t OSABI) {
  return std::make_unique<GPUMinELFObjectWriter>(OSABI);
}
