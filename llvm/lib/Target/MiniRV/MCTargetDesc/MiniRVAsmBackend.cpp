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

using namespace llvm;

std::unique_ptr<MCObjectTargetWriter>
MiniRVAsmBackend::createObjectTargetWriter() const {
  return createMiniRVELFObjectWriter(OSABI);
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
