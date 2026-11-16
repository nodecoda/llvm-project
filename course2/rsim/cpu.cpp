//===-- cpu.cpp - reset and the decode/execute loop, chapters 02-04 -------===//
//
// Every instruction is one step: fetch a word from pc, decode it, compute the
// result, write it back through setReg() (which drops x0 writes), and move pc.
//
// Chapter 04 makes the traps precise: a misaligned target, a misaligned load
// or store, an unmapped page and an unknown encoding all raise a Trap that
// carries (cause, tval, epc) where epc is the address of the *faulting*
// instruction.  Chapter 08 copies exactly these fields into the CSRs.
//
//===----------------------------------------------------------------------===//

#include "cpu.h"
#include "decode.h"

#include <cstdio>
#include <limits>

namespace rsim {

namespace {

uint32_t sext8(uint8_t v) {
  return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(v)));
}
uint32_t sext16(uint16_t v) {
  return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(v)));
}

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
  exit_code_ = 0;
}

uint8_t Cpu::load8(uint32_t addr, uint32_t epc) {
  try {
    return memory().read8(addr);
  } catch (const MemoryError &err) {
    throw Trap(TrapCause::LoadAccessFault, addr, epc, err.what());
  }
}

uint16_t Cpu::load16(uint32_t addr, uint32_t epc) {
  try {
    return memory().read16(addr);
  } catch (const MemoryError &err) {
    throw Trap(TrapCause::LoadAccessFault, addr, epc, err.what());
  }
}

uint32_t Cpu::load32(uint32_t addr, uint32_t epc) {
  try {
    return memory().read32(addr);
  } catch (const MemoryError &err) {
    throw Trap(TrapCause::LoadAccessFault, addr, epc, err.what());
  }
}

void Cpu::store8(uint32_t addr, uint8_t value, uint32_t epc) {
  try {
    memory().write8(addr, value);
  } catch (const MemoryError &err) {
    throw Trap(TrapCause::StoreAccessFault, addr, epc, err.what());
  }
}

void Cpu::store16(uint32_t addr, uint16_t value, uint32_t epc) {
  try {
    memory().write16(addr, value);
  } catch (const MemoryError &err) {
    throw Trap(TrapCause::StoreAccessFault, addr, epc, err.what());
  }
}

void Cpu::store32(uint32_t addr, uint32_t value, uint32_t epc) {
  try {
    memory().write32(addr, value);
  } catch (const MemoryError &err) {
    throw Trap(TrapCause::StoreAccessFault, addr, epc, err.what());
  }
}

bool Cpu::step() {
  if (halted_)
    return false;

  const uint32_t at = pc;
  if (at & 3u)
    throw Trap(TrapCause::InstructionAddressMisaligned, at, at,
               "pc is not 4-byte aligned");

  uint32_t word = 0;
  try {
    word = memory().read32(at);
  } catch (const MemoryError &err) {
    throw Trap(TrapCause::InstructionAccessFault, at, at, err.what());
  }

  const Decoded d = decode(word);
  if (d.id == Insn::ILLEGAL)
    throw Trap(TrapCause::IllegalInstruction, word, at,
               "no RV32I/M instruction matches");
  ++executed_;

  uint32_t next = at + 4;
  const uint32_t a = reg(d.rs1);
  const uint32_t b = reg(d.rs2);
  const uint32_t imm = static_cast<uint32_t>(d.imm);
  const uint32_t addr = a + imm;

  switch (d.id) {
  case Insn::ILLEGAL:
    break; // not reachable: rejected above

  case Insn::LUI: // decode() already zeroed the low 12 bits
    setReg(d.rd, imm);
    break;
  case Insn::AUIPC:
    setReg(d.rd, at + imm);
    break;
  case Insn::JAL: {
    const uint32_t target = at + imm;
    if (target & 3u)
      throw Trap(TrapCause::InstructionAddressMisaligned, target, at,
                 "jal target");
    setReg(d.rd, at + 4);
    next = target;
    break;
  }
  case Insn::JALR: {
    const uint32_t target = (a + imm) & ~1u; // the LSB is never part of it
    if (target & 3u)
      throw Trap(TrapCause::InstructionAddressMisaligned, target, at,
                 "jalr target");
    setReg(d.rd, at + 4);
    next = target;
    break;
  }

  case Insn::BEQ:
  case Insn::BNE:
  case Insn::BLT:
  case Insn::BGE:
  case Insn::BLTU:
  case Insn::BGEU: {
    bool taken = false;
    switch (d.id) {
    case Insn::BEQ:
      taken = a == b;
      break;
    case Insn::BNE:
      taken = a != b;
      break;
    case Insn::BLT:
      taken = static_cast<int32_t>(a) < static_cast<int32_t>(b);
      break;
    case Insn::BGE:
      taken = static_cast<int32_t>(a) >= static_cast<int32_t>(b);
      break;
    case Insn::BLTU:
      taken = a < b;
      break;
    default:
      taken = a >= b;
      break;
    }
    if (!taken)
      break;
    const uint32_t target = at + imm;
    if (target & 3u)
      throw Trap(TrapCause::InstructionAddressMisaligned, target, at,
                 "branch target");
    next = target;
    break;
  }

  case Insn::LB:
    setReg(d.rd, sext8(load8(addr, at)));
    break;
  case Insn::LH:
    if (addr & 1u)
      throw Trap(TrapCause::LoadAddressMisaligned, addr, at, "lh");
    setReg(d.rd, sext16(load16(addr, at)));
    break;
  case Insn::LW:
    if (addr & 3u)
      throw Trap(TrapCause::LoadAddressMisaligned, addr, at, "lw");
    setReg(d.rd, load32(addr, at));
    break;
  case Insn::LBU:
    setReg(d.rd, load8(addr, at));
    break;
  case Insn::LHU:
    if (addr & 1u)
      throw Trap(TrapCause::LoadAddressMisaligned, addr, at, "lhu");
    setReg(d.rd, load16(addr, at));
    break;

  case Insn::SB:
    store8(addr, static_cast<uint8_t>(b), at);
    break;
  case Insn::SH:
    if (addr & 1u)
      throw Trap(TrapCause::StoreAddressMisaligned, addr, at, "sh");
    store16(addr, static_cast<uint16_t>(b), at);
    break;
  case Insn::SW:
    if (addr & 3u)
      throw Trap(TrapCause::StoreAddressMisaligned, addr, at, "sw");
    store32(addr, b, at);
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
      setReg(d.rd, a); // the remainder is the dividend
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
    if (!ecall(at)) // exit(2) stops the loop; everything else returns
      return false;
    break;
  case Insn::EBREAK:
    // Chapter 08 turns this into the breakpoint trap the debugger sees.
    throw Trap(TrapCause::Breakpoint, at, at, "ebreak");
  }

  pc = next;
  return true;
}

} // namespace rsim
