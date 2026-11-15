//===-- RVMinELFObjectWriter.cpp - RVMin ELF writer -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A 32-bit little-endian ELF writer.  The e_machine field must say RISC-V
// (EM_RISCV), otherwise llvm-readelf/llvm-objdump will not recognize the
// output, even though the instruction encoding is otherwise correct.
//
// Relocations are introduced in chapter 7; until then an unresolved symbol
// produces a clear diagnostic instead of a silently wrong object file.
//
//===----------------------------------------------------------------------===//

#include "RVMinMCTargetDesc.h"
#include "llvm/ADT/Twine.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/MC/MCELFObjectWriter.h"
#include "llvm/MC/MCFixup.h"
#include "llvm/MC/MCObjectWriter.h"
#include "llvm/MC/MCValue.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

namespace {

class RVMinELFObjectWriter : public MCELFObjectTargetWriter {
public:
  explicit RVMinELFObjectWriter(uint8_t OSABI)
      : MCELFObjectTargetWriter(/*Is64Bit=*/false, OSABI, ELF::EM_RISCV,
                                /*HasRelocationAddend=*/true) {}
  ~RVMinELFObjectWriter() override = default;

protected:
  unsigned getRelocType(const MCFixup &Fixup, const MCValue &Target,
                        bool IsPCRel) const override {
    reportError(Fixup.getLoc(),
                "RVMin: relocations are not implemented yet (chapter 7)");
    return ELF::R_RISCV_NONE;
  }
};

} // end anonymous namespace

std::unique_ptr<MCObjectTargetWriter>
llvm::createRVMinELFObjectWriter(uint8_t OSABI) {
  return std::make_unique<RVMinELFObjectWriter>(OSABI);
}
