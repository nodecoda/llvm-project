//===-- MiniRVELFObjectWriter.cpp - MiniRV ELF writer ---------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "MiniRVMCTargetDesc.h"
#include "llvm/MC/MCELFObjectWriter.h"
#include "llvm/MC/MCFixup.h"
#include "llvm/MC/MCObjectWriter.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

namespace {
class MiniRVELFObjectWriter : public MCELFObjectTargetWriter {
public:
  explicit MiniRVELFObjectWriter(uint8_t OSABI)
      : MCELFObjectTargetWriter(/*Is64Bit=*/true, OSABI, ELF::EM_RISCV,
                                /*HasRelocationAddend=*/true) {}

protected:
  unsigned getRelocType(const MCFixup &Fixup, const MCValue &,
                        bool) const override {
    switch (Fixup.getKind()) {
    case MiniRV::fixup_minirv_branch:
      return ELF::R_RISCV_BRANCH;
    case MiniRV::fixup_minirv_jal:
      return ELF::R_RISCV_JAL;
    case MiniRV::fixup_minirv_pcrel_hi20:
      return ELF::R_RISCV_PCREL_HI20;
    case MiniRV::fixup_minirv_pcrel_lo12_i:
      return ELF::R_RISCV_PCREL_LO12_I;
    // Data relocations: a pointer in an initialiser refers to a symbol.
    case FK_Data_8:
      return ELF::R_RISCV_64;
    case FK_Data_4:
      return ELF::R_RISCV_32;
    default:
      llvm_unreachable("Unknown fixup kind!");
    }
  }

  bool needsRelocateWithSymbol(const MCValue &, unsigned) const override {
    // Keep the original symbol: the low part of a PC-relative pair has to name
    // the auipc label, not the section it lives in.
    return true;
  }
};
} // end anonymous namespace

std::unique_ptr<MCObjectTargetWriter>
llvm::createMiniRVELFObjectWriter(uint8_t OSABI) {
  return std::make_unique<MiniRVELFObjectWriter>(OSABI);
}
