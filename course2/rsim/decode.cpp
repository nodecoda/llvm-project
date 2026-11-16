//===-- decode.cpp - RV32 instruction decoder, chapter 03 ------------------===//

#include "decode.h"

#include <cctype>

namespace rsim {
namespace {

struct Entry {
  uint8_t opcode;
  uint8_t funct3;
  uint8_t funct7;
  uint16_t match_imm; // 0xff means "don't care"; mret's 0x302 does not fit u8
  uint8_t match_f5;   // funct5, for the A extension
  Insn id;
};

constexpr uint8_t kWild = 0xff;

const Entry kTable[] = {
#define RSIM_INSN(ID, MNEM, FMT, OPC, F3, F7, IMM, ISA, F5) \
  {OPC, F3, F7, IMM, F5, Insn::ID},
#include "insn.def"
#undef RSIM_INSN
};

int32_t signExtend(uint32_t value, unsigned bits) {
  const uint32_t sign = 1u << (bits - 1);
  return static_cast<int32_t>((value ^ sign) - sign);
}

uint32_t bits(uint32_t word, unsigned hi, unsigned lo) {
  return (word >> lo) & ((1u << (hi - lo + 1)) - 1);
}

int32_t decodeB(uint32_t word) {
  const uint32_t imm = (bits(word, 31, 31) << 12) | (bits(word, 7, 7) << 11) |
                       (bits(word, 30, 25) << 5) | (bits(word, 11, 8) << 1);
  return signExtend(imm, 13);
}

int32_t decodeJ(uint32_t word) {
  const uint32_t imm = (bits(word, 31, 31) << 20) | (bits(word, 19, 12) << 12) |
                       (bits(word, 20, 20) << 11) | (bits(word, 30, 21) << 1);
  return signExtend(imm, 21);
}

} // namespace

Decoded decode(uint32_t word) {
  Decoded out;
  out.raw = word;

  const uint8_t opcode = static_cast<uint8_t>(word & 0x7f);
  const uint8_t funct3 = static_cast<uint8_t>(bits(word, 14, 12));
  const uint8_t funct7 = static_cast<uint8_t>(bits(word, 31, 25));
  const int32_t imm12 = signExtend(bits(word, 31, 20), 12);

  for (const Entry &entry : kTable) {
    if (entry.opcode != opcode)
      continue;
    if (entry.funct3 != kWild && entry.funct3 != funct3)
      continue;
    if (entry.funct7 != kWild && entry.funct7 != funct7)
      continue;
    if (entry.match_imm != kWild && entry.match_imm != bits(word, 31, 20))
      continue;
    if (entry.match_f5 != kWild && entry.match_f5 != bits(word, 31, 27))
      continue;

    out.id = entry.id;
    out.rd = static_cast<uint8_t>(bits(word, 11, 7));
    out.rs1 = static_cast<uint8_t>(bits(word, 19, 15));
    out.rs2 = static_cast<uint8_t>(bits(word, 24, 20));
    break;
  }

  if (out.id == Insn::ILLEGAL)
    return out;

  switch (out.id) {
  case Insn::LUI:
  case Insn::AUIPC:
    out.fmt = Fmt::U;
    out.imm = static_cast<int32_t>(word & 0xfffff000);
    break;
  case Insn::JAL:
    out.fmt = Fmt::J;
    out.imm = decodeJ(word);
    break;
  case Insn::JALR:
    out.fmt = Fmt::Jalr;
    out.imm = imm12;
    break;
  case Insn::BEQ:
  case Insn::BNE:
  case Insn::BLT:
  case Insn::BGE:
  case Insn::BLTU:
  case Insn::BGEU:
    out.fmt = Fmt::Branch;
    out.imm = decodeB(word);
    break;
  case Insn::ADDI:
  case Insn::SLTI:
  case Insn::SLTIU:
  case Insn::XORI:
  case Insn::ORI:
  case Insn::ANDI:
    out.fmt = Fmt::I;
    out.imm = imm12;
    break;
  case Insn::LB:
  case Insn::LH:
  case Insn::LW:
  case Insn::LBU:
  case Insn::LHU:
    out.fmt = Fmt::Load;
    out.imm = imm12;
    break;
  case Insn::SB:
  case Insn::SH:
  case Insn::SW:
    out.fmt = Fmt::Store;
    out.imm = signExtend((bits(word, 31, 25) << 5) | bits(word, 11, 7), 12);
    break;
  case Insn::SLLI:
  case Insn::SRLI:
  case Insn::SRAI:
    out.fmt = Fmt::Shift;
    out.imm = static_cast<int32_t>(bits(word, 24, 20));
    break;
  case Insn::CSRRW:
  case Insn::CSRRS:
  case Insn::CSRRC:
  case Insn::CSRRWI:
  case Insn::CSRRSI:
  case Insn::CSRRCI:
  case Insn::MRET:
    out.fmt = Fmt::Csr;
    out.csr = static_cast<uint16_t>(bits(word, 31, 20));
    break;
  case Insn::CLZ:
  case Insn::CTZ:
  case Insn::CPOP:
  case Insn::SEXT_B:
  case Insn::SEXT_H:
  case Insn::REV8:
  case Insn::ZEXT_H:
    out.fmt = Fmt::Unary;
    break;
  case Insn::RORI:
  case Insn::BSETI:
  case Insn::BCLRI:
  case Insn::BINVI:
  case Insn::BEXTI:
    out.fmt = Fmt::Shift;
    out.imm = static_cast<int32_t>(bits(word, 24, 20));
    break;
  case Insn::LR_W:
  case Insn::SC_W:
  case Insn::AMOSWAP_W:
  case Insn::AMOADD_W:
  case Insn::AMOXOR_W:
  case Insn::AMOAND_W:
  case Insn::AMOOR_W:
  case Insn::AMOMIN_W:
  case Insn::AMOMAX_W:
  case Insn::AMOMINU_W:
  case Insn::AMOMAXU_W:
    out.fmt = Fmt::Atomic;
    break;
  case Insn::FENCE:
  case Insn::FENCE_I:
    out.fmt = Fmt::Fence;
    break;
  case Insn::ECALL:
  case Insn::EBREAK:
    out.fmt = Fmt::System;
    break;
  default:
    out.fmt = Fmt::R;
    break;
  }
  return out;
}

const char *mnemonic(Insn id) {
#define RSIM_INSN(ID, MNEM, FMT, OPC, F3, F7, IMM, ISA, F5) \
  if (id == Insn::ID)                                        \
    return MNEM;
#include "insn.def"
#undef RSIM_INSN
  return "ILLEGAL";
}

Isa isaOf(Insn id) {
#define RSIM_INSN(ID, MNEM, FMT, OPC, F3, F7, IMM, ISA, F5) \
  if (id == Insn::ID)                                        \
    return Isa::ISA;
#include "insn.def"
#undef RSIM_INSN
  return Isa::RV32I;
}

const char *csrName(uint16_t csr) {
  switch (csr) {
  case 0x300:
    return "mstatus";
  case 0x305:
    return "mtvec";
  case 0x341:
    return "mepc";
  case 0x342:
    return "mcause";
  case 0x343:
    return "mtval";
  case 0xb02:
    return "minstret";
  default:
    return nullptr;
  }
}

bool isImmediateCsr(Insn id) {
  return id == Insn::CSRRWI || id == Insn::CSRRSI || id == Insn::CSRRCI;
}

const char *registerName(unsigned reg) {
  static const char *const kNames[32] = {
      "zero", "ra", "sp", "gp", "tp",  "t0",  "t1",  "t2",
      "s0",   "s1", "a0", "a1", "a2",  "a3",  "a4",  "a5",
      "a6",   "a7", "s2", "s3", "s4",  "s5",  "s6",  "s7",
      "s8",   "s9", "s10", "s11", "t3", "t4",  "t5",  "t6"};
  return kNames[reg & 31];
}

std::string format(const Decoded &d) {
  const std::string m = mnemonic(d.id);
  const std::string rd = registerName(d.rd);
  const std::string rs1 = registerName(d.rs1);
  const std::string rs2 = registerName(d.rs2);

  switch (d.fmt) {
  case Fmt::R:
    return m + " " + rd + ", " + rs1 + ", " + rs2;
  case Fmt::I:
    return m + " " + rd + ", " + rs1 + ", " + std::to_string(d.imm);
  case Fmt::Shift:
    return m + " " + rd + ", " + rs1 + ", " + std::to_string(d.imm);
  case Fmt::Load:
    return m + " " + rd + ", " + std::to_string(d.imm) + "(" + rs1 + ")";
  case Fmt::Store:
    return m + " " + rs2 + ", " + std::to_string(d.imm) + "(" + rs1 + ")";
  case Fmt::Branch:
    return m + " " + rs1 + ", " + rs2 + ", " + std::to_string(d.imm);
  case Fmt::U:
    return m + " " + rd + ", " +
           std::to_string(static_cast<uint32_t>(d.imm) >> 12);
  case Fmt::J:
    return m + " " + rd + ", " + std::to_string(d.imm);
  case Fmt::Jalr:
    return m + " " + rd + ", " + std::to_string(d.imm) + "(" + rs1 + ")";
  case Fmt::Csr: {
    if (d.id == Insn::MRET)
      return m;
    const char *name = csrName(d.csr);
    const std::string csr =
        name ? std::string(name) : "0x" + std::to_string(d.csr);
    const std::string src = isImmediateCsr(d.id) ? std::to_string(d.rs1) : rs1;
    return m + " " + rd + ", " + csr + ", " + src;
  }
  case Fmt::Unary:
    return m + " " + rd + ", " + rs1;
  case Fmt::Atomic:
    if (d.id == Insn::LR_W)
      return m + " " + rd + ", (" + rs1 + ")";
    return m + " " + rd + ", " + rs2 + ", (" + rs1 + ")";
  case Fmt::Fence:
  case Fmt::System:
    return m;
  }
  return m;
}

} // namespace rsim
