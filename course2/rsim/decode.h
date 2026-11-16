//===-- decode.h - RV32 instruction decoder, chapter 03 ---------*- C++ -*-===//
//
// The decoder knows the instruction set; the CPU decides whether it may run
// (chapter 09 adds --isa filtering).
//
//===----------------------------------------------------------------------===//

#ifndef RSIM_DECODE_H
#define RSIM_DECODE_H

#include <cstdint>
#include <string>

namespace rsim {

enum class Fmt : uint8_t {
  R,      // add rd, rs1, rs2
  I,      // addi rd, rs1, imm
  Shift,  // slli rd, rs1, shamt
  Load,   // lw rd, imm(rs1)
  Store,  // sw rs2, imm(rs1)
  Branch, // beq rs1, rs2, offset
  U,      // lui rd, imm20
  J,      // jal rd, offset
  Jalr,   // jalr rd, imm(rs1)
  Fence,  // fence / fence.i
  System, // ecall / ebreak
  Csr,    // csrrw/csrrs/csrrc + immediate forms, plus mret
};

enum class Isa : uint8_t {
  RV32I = 1 << 0,
  RV32M = 1 << 1,
};

enum class Insn : uint16_t {
  ILLEGAL,
#define RSIM_INSN(ID, MNEM, FMT, OPC, F3, F7, IMM, ISA) ID,
#include "insn.def"
#undef RSIM_INSN
};

struct Decoded {
  Insn id = Insn::ILLEGAL;
  Fmt fmt = Fmt::R;
  uint8_t rd = 0;
  uint8_t rs1 = 0;
  uint8_t rs2 = 0;
  int32_t imm = 0;
  uint16_t csr = 0; // chapter 08: the CSR number of a Zicsr instruction
  uint32_t raw = 0;
};

Decoded decode(uint32_t word);

const char *mnemonic(Insn id);
Isa isaOf(Insn id);

/// The exact text llvm-mc prints for the same encoding, e.g.
/// "add a0, a1, a2" or "lw a0, -4(sp)".
std::string format(const Decoded &insn);

/// "mtvec", "mepc", ... for the CSRs the course implements.
const char *csrName(uint16_t csr);

/// True for csrrwi/csrrsi/csrrci, whose rs1 field is a 5-bit immediate.
bool isImmediateCsr(Insn id);

/// Register names in ABI order: zero, ra, sp, gp, tp, t0-t6, s0-s11, a0-a7.
const char *registerName(unsigned reg);

} // namespace rsim

#endif // RSIM_DECODE_H
