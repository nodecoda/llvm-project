//===-- GPUMinDisassembler.cpp - Disassembler for GPUMin ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Chapter 02: the inverse of the encoder.
//
// The decode tree itself is TableGen-generated from the same `Inst{}` fields
// the encoder uses (-gen-disassembler), so `llvm-mc -show-encoding` and
// `llvm-mc -disassemble` read one description.  What the generator cannot
// know is what a field *means*:
//
//   * a 5-bit register field indexes the per-lane file (r0-r31);
//   * a 3-bit field indexes the uniform file (ur0-ur7) or the predicate file
//     (p0-p7), depending on which register class the operand belongs to;
//   * a 16-bit immediate field is signed, so it must be sign-extended before
//     it is printed.
//
// Those three hooks are named from the .td side (`let DecoderMethod = ...`)
// and implemented below.  Everything else is generated.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "gpumin-disassembler"

#include "MCTargetDesc/GPUMinMCTargetDesc.h"
#include "TargetInfo/GPUMinTargetInfo.h"
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

// The three register files are contiguous in GPUMinRegisterInfo.td, so each
// one is a table indexed by the encoded field rather than a switch.
static const unsigned GPRDecoderTable[] = {
    GPUMin::R0,  GPUMin::R1,  GPUMin::R2,  GPUMin::R3,  GPUMin::R4,
    GPUMin::R5,  GPUMin::R6,  GPUMin::R7,  GPUMin::R8,  GPUMin::R9,
    GPUMin::R10, GPUMin::R11, GPUMin::R12, GPUMin::R13, GPUMin::R14,
    GPUMin::R15, GPUMin::R16, GPUMin::R17, GPUMin::R18, GPUMin::R19,
    GPUMin::R20, GPUMin::R21, GPUMin::R22, GPUMin::R23, GPUMin::R24,
    GPUMin::R25, GPUMin::R26, GPUMin::R27, GPUMin::R28, GPUMin::R29,
    GPUMin::R30, GPUMin::R31};

static const unsigned URegDecoderTable[] = {GPUMin::UR0, GPUMin::UR1,
                                            GPUMin::UR2, GPUMin::UR3,
                                            GPUMin::UR4, GPUMin::UR5,
                                            GPUMin::UR6, GPUMin::UR7};

static const unsigned PredDecoderTable[] = {GPUMin::P0, GPUMin::P1,
                                            GPUMin::P2, GPUMin::P3,
                                            GPUMin::P4, GPUMin::P5,
                                            GPUMin::P6, GPUMin::P7};

static DecodeStatus DecodeGPRRegisterClass(MCInst &Inst, uint64_t RegNo,
                                           uint64_t Address,
                                           const MCDisassembler *Decoder) {
  if (RegNo >= std::size(GPRDecoderTable))
    return MCDisassembler::Fail;
  Inst.addOperand(MCOperand::createReg(GPRDecoderTable[RegNo]));
  return MCDisassembler::Success;
}

static DecodeStatus DecodeUGPRRegisterClass(MCInst &Inst, uint64_t RegNo,
                                            uint64_t Address,
                                            const MCDisassembler *Decoder) {
  if (RegNo >= std::size(URegDecoderTable))
    return MCDisassembler::Fail;
  Inst.addOperand(MCOperand::createReg(URegDecoderTable[RegNo]));
  return MCDisassembler::Success;
}

static DecodeStatus DecodePREDRegisterClass(MCInst &Inst, uint64_t RegNo,
                                            uint64_t Address,
                                            const MCDisassembler *Decoder) {
  if (RegNo >= std::size(PredDecoderTable))
    return MCDisassembler::Fail;
  Inst.addOperand(MCOperand::createReg(PredDecoderTable[RegNo]));
  return MCDisassembler::Success;
}

// A 16-bit field that the ISA defines as signed.  Without this the printer
// would read `movi r1, -1` back as 65535, and the assemble/disassemble round
// trip would stop being an identity.
static DecodeStatus DecodeSImm16(MCInst &Inst, uint64_t Imm, int64_t Address,
                                 const MCDisassembler *Decoder) {
  Inst.addOperand(MCOperand::createImm(SignExtend64<16>(Imm)));
  return MCDisassembler::Success;
}

// Branch displacements are stored in instruction words.  The printer and the
// assembler both think in words (`bra 2`), but the symbolizer needs a byte
// delta from the branch address, so scale before offering it the target.
static DecodeStatus DecodeBrTarget16(MCInst &Inst, uint64_t Imm,
                                     int64_t Address,
                                     const MCDisassembler *Decoder) {
  const int64_t Words = SignExtend64<16>(Imm);
  if (!Decoder->tryAddingSymbolicOperand(Inst, Words * 4, Address,
                                         /*IsBranch=*/true, /*Offset=*/0,
                                         /*OpSize=*/4, /*InstSize=*/4))
    Inst.addOperand(MCOperand::createImm(Words));
  return MCDisassembler::Success;
}

#include "GPUMinGenDisassemblerTables.inc"

DecodeStatus GPUMinDisassembler::getInstruction(MCInst &MI, uint64_t &Size,
                                               ArrayRef<uint8_t> Bytes,
                                               uint64_t Address,
                                               raw_ostream &CStream) const {
  Size = 0;
  if (Bytes.size() < 4)
    return MCDisassembler::Fail;

  uint32_t Insn = support::endian::read32le(Bytes.data());
  DecodeStatus Result =
      decodeInstruction(DecoderTable32, MI, Insn, Address, this, STI);

  // Every GM32 instruction is one word.  Reporting Size = 4 even for a word
  // the table does not know keeps objdump from retrying at +1/+2/+3 and
  // printing four `<unknown>` lines for one bad instruction.
  Size = 4;
  return Result;
}
