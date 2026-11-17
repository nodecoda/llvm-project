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
// Chapter 7 adds the relocations a static RV32I object needs: absolute 32-bit
// data (R_RISCV_32), the %hi/%lo global-address pair, and the branch/jump
// types that only appear when a target is outside the current section.
//
//===----------------------------------------------------------------------===//

#include "RVMinFixupKinds.h"
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
    switch (Fixup.getKind()) {
    case FK_Data_4:
      return IsPCRel ? ELF::R_RISCV_32_PCREL : ELF::R_RISCV_32;
    case rvmin::fixup_rvmin_32:
      return ELF::R_RISCV_32;
    case rvmin::fixup_rvmin_branch:
      return ELF::R_RISCV_BRANCH;
    case rvmin::fixup_rvmin_jal:
      return ELF::R_RISCV_JAL;
    case rvmin::fixup_rvmin_hi20:
      return ELF::R_RISCV_HI20;
    case rvmin::fixup_rvmin_lo12_i:
      return ELF::R_RISCV_LO12_I;
    case rvmin::fixup_rvmin_lo12_s:
      return ELF::R_RISCV_LO12_S;
    case rvmin::fixup_rvmin_rvc_branch:
      return ELF::R_RISCV_RVC_BRANCH;
    case rvmin::fixup_rvmin_rvc_jump:
      return ELF::R_RISCV_RVC_JUMP;
    case rvmin::fixup_rvmin_rvc_imm:
      reportError(Fixup.getLoc(),
                  "RVMin: compressed instructions have no immediate relocation");
      return ELF::R_RISCV_NONE;
    default:
      reportError(Fixup.getLoc(), "RVMin: unsupported relocation");
      return ELF::R_RISCV_NONE;
    }
  }
};

} // end anonymous namespace

std::unique_ptr<MCObjectTargetWriter>
llvm::createRVMinELFObjectWriter(uint8_t OSABI) {
  return std::make_unique<RVMinELFObjectWriter>(OSABI);
}
