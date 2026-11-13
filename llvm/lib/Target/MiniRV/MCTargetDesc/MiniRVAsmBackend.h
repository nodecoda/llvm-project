//===-- MiniRVAsmBackend.h - MiniRV assembler backend -----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_MINIRV_MCTARGETDESC_MINIRVASMBACKEND_H
#define LLVM_LIB_TARGET_MINIRV_MCTARGETDESC_MINIRVASMBACKEND_H

#include "MiniRVMCTargetDesc.h"
#include "llvm/MC/MCAsmBackend.h"

namespace llvm {

class MiniRVAsmBackend : public MCAsmBackend {
  uint8_t OSABI;

public:
  MiniRVAsmBackend(uint8_t OSABI)
      : MCAsmBackend(llvm::endianness::little), OSABI(OSABI) {}

  MCFixupKindInfo getFixupKindInfo(MCFixupKind Kind) const override;

  void applyFixup(const MCFragment &F, const MCFixup &Fixup,
                  const MCValue &Target, uint8_t *Data, uint64_t Value,
                  bool IsResolved) override;

  std::optional<bool> evaluateFixup(const MCFragment &F, MCFixup &Fixup,
                                    MCValue &Target,
                                    uint64_t &Value) override;

  std::unique_ptr<MCObjectTargetWriter>
  createObjectTargetWriter() const override;

  bool writeNopData(raw_ostream &OS, uint64_t Count,
                    const MCSubtargetInfo *STI) const override;
};

} // namespace llvm

#endif
