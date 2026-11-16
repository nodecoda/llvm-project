//===-- cpu.cpp - reset and the decode/execute loop, chapters 02-03 -------===//
//
// Every instruction is one step: fetch a word from pc, decode it, compute the
// result, write it back through setReg() (which drops x0 writes), and move pc.
// Loads and stores go through Memory, so an unmapped address is a trap rather
// than a silent zero.
//
//===----------------------------------------------------------------------===//

#include "cpu.h"
#include "decode.h"

#include <cstdarg>
#include <cstdio>
#include <limits>

namespace rsim {

namespace {

[[noreturn]] void trapf(const char *fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  throw Trap(buf);
}

uint32_t sext8(uint8_t v) { return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(v))); }
uint32_t sext16(uint16_t v) { return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(v))); }

} // namespace

void Cpu::reset(const Image &image, Memory &memory) {
  memory_ = &memory;
  memory.load(image);
  memory.map(kStackBase, kStackSize);

  for (uint32_t &reg : x)
    reg = 0;
  pc = image.entry;
  x[2] = kStackTop; // sp
  halted_ = false;
  executed_ = 0;
}

bool Cpu::step() {
  if (halted_)
    return false;

  const uint32_t at = pc;
  const uint32_t word = memory().read32(at);
  const Decoded d = decode(word);
  if (d.id == Insn::ILLEGAL)
    trapf("illegal instruction 0x%08x at 0x%08x", word, at);
  ++executed_;

  uint32_t next = at + 4;
  const uint32_t a = reg(d.rs1);
  const uint32_t b = reg(d.rs2);
  const uint32_t imm = static_cast<uint32_t>(d.imm);
  const uint32_t addr = a + imm;

  switch (d.id) {
  case Insn::ILLEGAL:
    break; // not reachable: rejected above

  case Insn::LUI:   // decode() already zeroed the low 12 bits
    setReg(d.rd, imm);
    break;
  case Insn::AUIPC:
    setReg(d.rd, at + imm);
    break;
  case Insn::JAL:
    setReg(d.rd, at + 4);
    next = at + imm;
    break;
  case Insn::JALR: {
    const uint32_t target = (a + imm) & ~1u; // the LSB is never part of it
    setReg(d.rd, at + 4);
    next = target;
    break;
  }

  case Insn::BEQ:
    if (a == b)
      next = at + imm;
    break;
  case Insn::BNE:
    if (a != b)
      next = at + imm;
    break;
  case Insn::BLT:
    if (static_cast<int32_t>(a) < static_cast<int32_t>(b))
      next = at + imm;
    break;
  case Insn::BGE:
    if (static_cast<int32_t>(a) >= static_cast<int32_t>(b))
      next = at + imm;
    break;
  case Insn::BLTU:
    if (a < b)
      next = at + imm;
    break;
  case Insn::BGEU:
    if (a >= b)
      next = at + imm;
    break;

  case Insn::LB:
    setReg(d.rd, sext8(memory().read8(addr)));
    break;
  case Insn::LH:
    setReg(d.rd, sext16(memory().read16(addr)));
    break;
  case Insn::LW:
    setReg(d.rd, memory().read32(addr));
    break;
  case Insn::LBU:
    setReg(d.rd, memory().read8(addr));
    break;
  case Insn::LHU:
    setReg(d.rd, memory().read16(addr));
    break;

  case Insn::SB:
    memory().write8(addr, static_cast<uint8_t>(b));
    break;
  case Insn::SH:
    memory().write16(addr, static_cast<uint16_t>(b));
    break;
  case Insn::SW:
    memory().write32(addr, b);
    break;

  case Insn::ADDI:
    setReg(d.rd, a + imm);
    break;
  case Insn::SLTI:
    setReg(d.rd, static_cast<int32_t>(a) < d.imm ? 1u : 0u);
    break;
  case Insn::SLTIU: // the immediate is sign-extended, then compared unsigned
    setReg(d.rd, a < imm ? 1u : 0u);
    break;
  case Insn::XORI:
    setReg(d.rd, a ^ imm);
    break;
  case Insn::ORI:
    setReg(d.rd, a | imm);
    break;
  case Insn::ANDI:
    setReg(d.rd, a & imm);
    break;

  case Insn::SLLI:
    setReg(d.rd, a << (imm & 31));
    break;
  case Insn::SRLI:
    setReg(d.rd, a >> (imm & 31));
    break;
  case Insn::SRAI:
    setReg(d.rd, static_cast<uint32_t>(static_cast<int32_t>(a) >> (imm & 31)));
    break;

  case Insn::ADD:
    setReg(d.rd, a + b);
    break;
  case Insn::SUB:
    setReg(d.rd, a - b);
    break;
  case Insn::SLL:
    setReg(d.rd, a << (b & 31));
    break;
  case Insn::SLT:
    setReg(d.rd, static_cast<int32_t>(a) < static_cast<int32_t>(b) ? 1u : 0u);
    break;
  case Insn::SLTU:
    setReg(d.rd, a < b ? 1u : 0u);
    break;
  case Insn::XOR:
    setReg(d.rd, a ^ b);
    break;
  case Insn::SRL:
    setReg(d.rd, a >> (b & 31));
    break;
  case Insn::SRA:
    setReg(d.rd, static_cast<uint32_t>(static_cast<int32_t>(a) >> (b & 31)));
    break;
  case Insn::OR:
    setReg(d.rd, a | b);
    break;
  case Insn::AND:
    setReg(d.rd, a & b);
    break;

  case Insn::MUL:
    setReg(d.rd, static_cast<uint32_t>(static_cast<uint64_t>(a) * b));
    break;
  case Insn::MULH:
    setReg(d.rd, static_cast<uint32_t>(
                      (static_cast<int64_t>(static_cast<int32_t>(a)) *
                       static_cast<int64_t>(static_cast<int32_t>(b))) >>
                      32));
    break;
  case Insn::MULHSU:
    setReg(d.rd, static_cast<uint32_t>(
                      (static_cast<int64_t>(static_cast<int32_t>(a)) *
                       static_cast<int64_t>(static_cast<uint64_t>(b))) >>
                      32));
    break;
  case Insn::MULHU:
    setReg(d.rd, static_cast<uint32_t>(
                      (static_cast<uint64_t>(a) * static_cast<uint64_t>(b)) >> 32));
    break;
  case Insn::DIV: {
    const int32_t sa = static_cast<int32_t>(a);
    const int32_t sb = static_cast<int32_t>(b);
    constexpr int32_t kMin = std::numeric_limits<int32_t>::min();
    if (sb == 0) // division by zero has a defined answer in RISC-V
      setReg(d.rd, 0xffffffffu);
    else if (sa == kMin && sb == -1) // overflow saturates, it does not trap
      setReg(d.rd, static_cast<uint32_t>(kMin));
    else
      setReg(d.rd, static_cast<uint32_t>(sa / sb));
    break;
  }
  case Insn::DIVU: {
    const uint32_t sa = a;
    const uint32_t sb = b;
    setReg(d.rd, sb == 0 ? 0xffffffffu : sa / sb);
    break;
  }
  case Insn::REM: {
    const int32_t sa = static_cast<int32_t>(a);
    const int32_t sb = static_cast<int32_t>(b);
    constexpr int32_t kMin = std::numeric_limits<int32_t>::min();
    if (sb == 0)
      setReg(d.rd, a); // remainder is the dividend
    else if (sa == kMin && sb == -1)
      setReg(d.rd, 0u);
    else
      setReg(d.rd, static_cast<uint32_t>(sa % sb));
    break;
  }
  case Insn::REMU: {
    const uint32_t sa = a;
    const uint32_t sb = b;
    setReg(d.rd, sb == 0 ? sa : sa % sb);
    break;
  }

  case Insn::FENCE:
  case Insn::FENCE_I:
    break; // single hart, single memory: nothing to order

  case Insn::ECALL:
  case Insn::EBREAK:
    // Chapter 03 only needs to stop; chapters 05 and 08 give these meaning.
    halted_ = true;
    return false;
  }

  pc = next;
  return true;
}

} // namespace rsim
