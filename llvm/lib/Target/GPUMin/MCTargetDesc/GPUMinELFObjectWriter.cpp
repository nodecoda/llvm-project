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
// Chapter 00 reuses EM_RISCV as the machine number so that the existing
// llvm-objdump build can still open the file while the tutorial has no
// disassembler of its own.  Chapter 03 revisits this: a distinct machine
// number (and the matching objdump wiring) is what keeps `llvm-objdump -d`
// from decoding GM32 bytes as RISC-V.
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
      : MCELFObjectTargetWriter(/*Is64Bit=*/false, OSABI, ELF::EM_RISCV,
                                /*HasRelocationAddend=*/true) {}
  ~GPUMinELFObjectWriter() override = default;

protected:
  unsigned getRelocType(const MCFixup &Fixup, const MCValue &Target,
                        bool IsPCRel) const override {
    switch (Fixup.getKind()) {
    case FK_Data_4:
      return IsPCRel ? ELF::R_RISCV_32_PCREL : ELF::R_RISCV_32;
    case gpumin::fixup_gpumin_32:
      return ELF::R_RISCV_32;
    default:
      reportError(Fixup.getLoc(), Twine("GPUMin: unsupported relocation"));
      return ELF::R_RISCV_NONE;
    }
  }
};

} // end anonymous namespace

std::unique_ptr<MCObjectTargetWriter>
llvm::createGPUMinELFObjectWriter(uint8_t OSABI) {
  return std::make_unique<GPUMinELFObjectWriter>(OSABI);
}
