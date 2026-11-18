//===-- GPUMinDisassembler.cpp - Disassembler for GPUMin ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The decoder is TableGen-generated from the same instruction definitions the
// encoder uses (-gen-disassembler), so the two can never drift apart.
//
// Chapter 00 has no instructions, so this file only installs the target hook
// and reports every word as undecodable.  Chapter 02 fills in the decoder
// methods and the asm -> obj -> disasm round trip.
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/GPUMinMCTargetDesc.h"
#include "TargetInfo/GPUMinTargetInfo.h"
#include "llvm/MC/MCDecoder.h"
#include "llvm/MC/MCDisassembler/MCDisassembler.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Endian.h"

using namespace llvm;

typedef MCDisassembler::DecodeStatus DecodeStatus;

namespace {

class GPUMinDisassembler : public MCDisassembler {
public:
  GPUMinDisassembler(const MCSubtargetInfo &STI, MCContext &Ctx)
      : MCDisassembler(STI, Ctx) {}

  DecodeStatus getInstruction(MCInst &MI, uint64_t &Size,
                              ArrayRef<uint8_t> Bytes, uint64_t Address,
                              raw_ostream &CStream) const override;
};

} // end anonymous namespace

static MCDisassembler *createGPUMinDisassembler(const Target &T,
                                                const MCSubtargetInfo &STI,
                                                MCContext &Ctx) {
  return new GPUMinDisassembler(STI, Ctx);
}

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializeGPUMinDisassembler() {
  TargetRegistry::RegisterMCDisassembler(getTheGPUMinTarget(),
                                         createGPUMinDisassembler);
}

DecodeStatus GPUMinDisassembler::getInstruction(MCInst &MI, uint64_t &Size,
                                                ArrayRef<uint8_t> Bytes,
                                                uint64_t Address,
                                                raw_ostream &CStream) const {
  if (Bytes.size() < 4) {
    Size = 0;
    return MCDisassembler::Fail;
  }
  Size = 4;

  uint32_t Insn = support::endian::read32le(Bytes.data());
  (void)Insn;
  // No opcodes are described yet (chapter 02).
  return MCDisassembler::Fail;
}
