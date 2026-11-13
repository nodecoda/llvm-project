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
  unsigned getRelocType(const MCFixup &, const MCValue &,
                        bool) const override {
    // No target fixups are created by this corpus.
    llvm_unreachable("MiniRV has no target fixups");
  }
};
} // end anonymous namespace

std::unique_ptr<MCObjectTargetWriter>
llvm::createMiniRVELFObjectWriter(uint8_t OSABI) {
  return std::make_unique<MiniRVELFObjectWriter>(OSABI);
}
