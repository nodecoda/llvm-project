//===-- RVMinDisassembler.cpp - Disassembler for RVMin --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Chapter 20: the inverse of the MC layer built in chapters 1-2.
//
// The decoder itself is TableGen-generated from the same instruction
// definitions that the encoder uses (`-gen-disassembler`), so the two can
// never drift apart: `RVMinGenDisassemblerTables.inc` reads the same
// `Inst{}` field layout as `RVMinGenMCCodeEmitter.inc`.
//
// What is *not* generated is how an operand's bits turn into a printable
// value.  Every signed immediate needs a decoder method:
//
//   * I- and S-type immediates are plain sign-extended 12-bit values;
//   * B- and J-type offsets are stored in the instruction word as scattered
//     bits (see RVMinInstrFormats.td) but the operand holds the *full byte
//     offset*, so the decoder just sign-extends the 13/21 bits that TableGen
//     already reassembled.
//
// Without those methods `addi a0, a0, -1` would disassemble as
// `addi a0, a0, 4095` and the asm -> obj -> disasm -> asm round trip in
// scripts/check-rvmin-disasm.sh would fail.
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/RVMinMCTargetDesc.h"
#include "TargetInfo/RVMinTargetInfo.h"
#include "llvm/MC/MCDecoder.h"
#include "llvm/MC/MCDecoderOps.h"
#include "llvm/MC/MCDisassembler/MCDisassembler.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/MathExtras.h"

using namespace llvm;
using namespace llvm::MCD;

#define DEBUG_TYPE "rvmin-disassembler"

typedef MCDisassembler::DecodeStatus DecodeStatus;

namespace {
class RVMinDisassembler : public MCDisassembler {
public:
  RVMinDisassembler(const MCSubtargetInfo &STI, MCContext &Ctx)
      : MCDisassembler(STI, Ctx) {}

  DecodeStatus getInstruction(MCInst &MI, uint64_t &Size,
                              ArrayRef<uint8_t> Bytes, uint64_t Address,
                              raw_ostream &CStream) const override;
};
} // end anonymous namespace

static MCDisassembler *createRVMinDisassembler(const Target &T,
                                               const MCSubtargetInfo &STI,
                                               MCContext &Ctx) {
  return new RVMinDisassembler(STI, Ctx);
}

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializeRVMinDisassembler() {
  TargetRegistry::RegisterMCDisassembler(getTheRVMinTarget(),
                                         createRVMinDisassembler);
}

// The register file is contiguous in RVMinRegisterInfo.td (X0..X31), so the
// decoder can index a table instead of a switch.  RegNo is the 5-bit field
// from the instruction word.
static const unsigned GPRDecoderTable[] = {
    RVMin::X0,  RVMin::X1,  RVMin::X2,  RVMin::X3,  RVMin::X4,  RVMin::X5,
    RVMin::X6,  RVMin::X7,  RVMin::X8,  RVMin::X9,  RVMin::X10, RVMin::X11,
    RVMin::X12, RVMin::X13, RVMin::X14, RVMin::X15, RVMin::X16, RVMin::X17,
    RVMin::X18, RVMin::X19, RVMin::X20, RVMin::X21, RVMin::X22, RVMin::X23,
    RVMin::X24, RVMin::X25, RVMin::X26, RVMin::X27, RVMin::X28, RVMin::X29,
    RVMin::X30, RVMin::X31};

static DecodeStatus DecodeGPRRegisterClass(MCInst &Inst, uint64_t RegNo,
                                           uint64_t Address,
                                           const MCDisassembler *Decoder) {
  if (RegNo >= std::size(GPRDecoderTable))
    return MCDisassembler::Fail;
  Inst.addOperand(MCOperand::createReg(GPRDecoderTable[RegNo]));
  return MCDisassembler::Success;
}

// Plain sign extension: I-type, S-type and JALR's 12-bit immediate.
template <unsigned N>
static DecodeStatus decodeSImmOperand(MCInst &Inst, uint64_t Imm,
                                      int64_t Address,
                                      const MCDisassembler *Decoder) {
  Inst.addOperand(MCOperand::createImm(SignExtend64<N>(Imm)));
  return MCDisassembler::Success;
}

// Branch/jump targets: the operand value is already the full byte offset
// (bit 0 is zero), so sign-extension is all that is left.  Offering it to the
// symbolizer lets `llvm-objdump` print labels instead of raw numbers.
template <unsigned N>
static DecodeStatus decodeSImmBranchOperand(MCInst &Inst, uint64_t Imm,
                                            int64_t Address,
                                            const MCDisassembler *Decoder) {
  const int64_t Value = SignExtend64<N>(Imm);
  if (!Decoder->tryAddingSymbolicOperand(Inst, Value, Address,
                                         /*IsBranch=*/true, /*Offset=*/0,
                                         /*OpSize=*/4, /*InstSize=*/4))
    Inst.addOperand(MCOperand::createImm(Value));
  return MCDisassembler::Success;
}

#include "RVMinGenDisassemblerTables.inc"

DecodeStatus RVMinDisassembler::getInstruction(MCInst &MI, uint64_t &Size,
                                               ArrayRef<uint8_t> Bytes,
                                               uint64_t Address,
                                               raw_ostream &CStream) const {
  Size = 0;
  // There is no C extension in RVMin: every instruction is exactly four
  // bytes, so a short tail cannot be decoded at all.
  if (Bytes.size() < 4)
    return MCDisassembler::Fail;

  uint64_t Insn = support::endian::read32le(Bytes.data());
  DecodeStatus Result =
      decodeInstruction(DecoderTable32, MI, Insn, Address, this, STI);
  // Every RVMin instruction is four bytes, so one undecodable word maps to
  // exactly one `.word` in the output.  Returning Size = 0 here would make
  // llvm-objdump retry at every byte offset and print four `<unknown>` lines
  // for a single invalid word.
  Size = 4;
  return Result;
}
