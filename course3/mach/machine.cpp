//===-- machine.cpp - fetch, decode, execute at 0x80000000, chapter 01 ------===//
//
// The instruction semantics are the ones course 2 already validated; what is
// new is where they live.  Every fetch and every data access goes through the
// Machine, so chapters 05-07 can insert the bus and Sv32 without touching the
// ALU cases.
//
// Chapter 01 stops on ebreak/ecall and reports a0 as the exit code.  Chapter
// 03 replaces that shortcut with the real trap model.
//
//===----------------------------------------------------------------------===//

#include "machine.h"

#include "decode.h"

#include <cinttypes>
#include <cstdio>
#include <limits>

namespace rmach {

namespace {

uint32_t sext8(uint8_t v) {
  return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(v)));
}
uint32_t sext16(uint16_t v) {
  return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(v)));
}

} // namespace

void Machine::reset(const rsim::Image &image, rsim::Memory &memory) {
  memory_ = &memory;
  memory.load(image);
  memory.map(kKernelStackTop - kKernelStackSize, kKernelStackSize);

  for (uint32_t &reg : x)
    reg = 0;
  pc = image.entry;
  x[2] = kKernelStackTop; // sp
  halted_ = false;
  executed_ = 0;
  exit_code_ = 0;
  trace_ = nullptr;
  isa_ = kIsaDefault;
}

uint8_t Machine::load8(uint32_t addr, uint32_t epc) {
  try {
    return memory_->read8(addr);
  } catch (const rsim::MemoryError &err) {
    throw Trap(TrapCause::LoadAccessFault, addr, epc, err.what());
  }
}

uint16_t Machine::load16(uint32_t addr, uint32_t epc) {
  try {
    return memory_->read16(addr);
  } catch (const rsim::MemoryError &err) {
    throw Trap(TrapCause::LoadAccessFault, addr, epc, err.what());
  }
}

uint32_t Machine::load32(uint32_t addr, uint32_t epc) {
  try {
    return memory_->read32(addr);
  } catch (const rsim::MemoryError &err) {
    throw Trap(TrapCause::LoadAccessFault, addr, epc, err.what());
  }
}

void Machine::store8(uint32_t addr, uint8_t value, uint32_t epc) {
  try {
    memory_->write8(addr, value);
  } catch (const rsim::MemoryError &err) {
    throw Trap(TrapCause::StoreAccessFault, addr, epc, err.what());
  }
}

void Machine::store16(uint32_t addr, uint16_t value, uint32_t epc) {
  try {
    memory_->write16(addr, value);
  } catch (const rsim::MemoryError &err) {
    throw Trap(TrapCause::StoreAccessFault, addr, epc, err.what());
  }
}

void Machine::store32(uint32_t addr, uint32_t value, uint32_t epc) {
  try {
    memory_->write32(addr, value);
  } catch (const rsim::MemoryError &err) {
    throw Trap(TrapCause::StoreAccessFault, addr, epc, err.what());
  }
}

bool Machine::step() {
  if (halted_)
    return false;
  executeOne();
  return !halted_;
}

bool Machine::executeOne() {
  const uint32_t at = pc;
  if (at & 3u)
    throw Trap(TrapCause::InstructionAddressMisaligned, at, at,
               "pc is not 4-byte aligned");

  uint32_t word = 0;
  try {
    word = memory_->read32(at);
  } catch (const rsim::MemoryError &err) {
    throw Trap(TrapCause::InstructionAccessFault, at, at, err.what());
  }

  const rsim::Decoded d = rsim::decode(word);
  if (d.id == rsim::Insn::ILLEGAL)
    throw Trap(TrapCause::IllegalInstruction, word, at,
               "no implemented instruction matches");
  if ((isa_ & static_cast<uint32_t>(rsim::isaOf(d.id))) == 0)
    throw Trap(TrapCause::IllegalInstruction, word, at,
               "instruction not enabled by the ISA mask");
  ++executed_;

  if (trace_)
    std::fprintf(trace_, "T %08" PRIx32 " %08" PRIx32 " %s\n", at, word,
                 rsim::format(d).c_str());

  uint32_t next = at + 4;
  const uint32_t a = reg(d.rs1);
  const uint32_t b = reg(d.rs2);
  const uint32_t imm = static_cast<uint32_t>(d.imm);
  const uint32_t addr = a + imm;

  switch (d.id) {
  case rsim::Insn::ILLEGAL:
    break; // rejected above

  case rsim::Insn::LUI:
    setReg(d.rd, imm);
    break;
  case rsim::Insn::AUIPC:
    setReg(d.rd, at + imm);
    break;
  case rsim::Insn::JAL: {
    const uint32_t target = at + imm;
    if (target & 3u)
      throw Trap(TrapCause::InstructionAddressMisaligned, target, at,
                 "jal target");
    setReg(d.rd, at + 4);
    next = target;
    break;
  }
  case rsim::Insn::JALR: {
    const uint32_t target = (a + imm) & ~1u;
    if (target & 3u)
      throw Trap(TrapCause::InstructionAddressMisaligned, target, at,
                 "jalr target");
    setReg(d.rd, at + 4);
    next = target;
    break;
  }

  case rsim::Insn::BEQ:
  case rsim::Insn::BNE:
  case rsim::Insn::BLT:
  case rsim::Insn::BGE:
  case rsim::Insn::BLTU:
  case rsim::Insn::BGEU: {
    bool taken = false;
    switch (d.id) {
    case rsim::Insn::BEQ:
      taken = a == b;
      break;
    case rsim::Insn::BNE:
      taken = a != b;
      break;
    case rsim::Insn::BLT:
      taken = static_cast<int32_t>(a) < static_cast<int32_t>(b);
      break;
    case rsim::Insn::BGE:
      taken = static_cast<int32_t>(a) >= static_cast<int32_t>(b);
      break;
    case rsim::Insn::BLTU:
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

  case rsim::Insn::LB:
    setReg(d.rd, sext8(load8(addr, at)));
    break;
  case rsim::Insn::LH:
    if (addr & 1u)
      throw Trap(TrapCause::LoadAddressMisaligned, addr, at, "lh");
    setReg(d.rd, sext16(load16(addr, at)));
    break;
  case rsim::Insn::LW:
    if (addr & 3u)
      throw Trap(TrapCause::LoadAddressMisaligned, addr, at, "lw");
    setReg(d.rd, load32(addr, at));
    break;
  case rsim::Insn::LBU:
    setReg(d.rd, load8(addr, at));
    break;
  case rsim::Insn::LHU:
    if (addr & 1u)
      throw Trap(TrapCause::LoadAddressMisaligned, addr, at, "lhu");
    setReg(d.rd, load16(addr, at));
    break;

  case rsim::Insn::SB:
    store8(addr, static_cast<uint8_t>(b), at);
    break;
  case rsim::Insn::SH:
    if (addr & 1u)
      throw Trap(TrapCause::StoreAddressMisaligned, addr, at, "sh");
    store16(addr, static_cast<uint16_t>(b), at);
    break;
  case rsim::Insn::SW:
    if (addr & 3u)
      throw Trap(TrapCause::StoreAddressMisaligned, addr, at, "sw");
    store32(addr, b, at);
    break;

  case rsim::Insn::ADDI:
    setReg(d.rd, a + imm);
    break;
  case rsim::Insn::SLTI:
    setReg(d.rd, static_cast<int32_t>(a) < d.imm ? 1u : 0u);
    break;
  case rsim::Insn::SLTIU:
    setReg(d.rd, a < imm ? 1u : 0u);
    break;
  case rsim::Insn::XORI:
    setReg(d.rd, a ^ imm);
    break;
  case rsim::Insn::ORI:
    setReg(d.rd, a | imm);
    break;
  case rsim::Insn::ANDI:
    setReg(d.rd, a & imm);
    break;

  case rsim::Insn::SLLI:
    setReg(d.rd, a << (imm & 31));
    break;
  case rsim::Insn::SRLI:
    setReg(d.rd, a >> (imm & 31));
    break;
  case rsim::Insn::SRAI:
    setReg(d.rd, static_cast<uint32_t>(static_cast<int32_t>(a) >> (imm & 31)));
    break;

  case rsim::Insn::ADD:
    setReg(d.rd, a + b);
    break;
  case rsim::Insn::SUB:
    setReg(d.rd, a - b);
    break;
  case rsim::Insn::SLL:
    setReg(d.rd, a << (b & 31));
    break;
  case rsim::Insn::SLT:
    setReg(d.rd, static_cast<int32_t>(a) < static_cast<int32_t>(b) ? 1u : 0u);
    break;
  case rsim::Insn::SLTU:
    setReg(d.rd, a < b ? 1u : 0u);
    break;
  case rsim::Insn::XOR:
    setReg(d.rd, a ^ b);
    break;
  case rsim::Insn::SRL:
    setReg(d.rd, a >> (b & 31));
    break;
  case rsim::Insn::SRA:
    setReg(d.rd, static_cast<uint32_t>(static_cast<int32_t>(a) >> (b & 31)));
    break;
  case rsim::Insn::OR:
    setReg(d.rd, a | b);
    break;
  case rsim::Insn::AND:
    setReg(d.rd, a & b);
    break;

  case rsim::Insn::MUL:
    setReg(d.rd, static_cast<uint32_t>(static_cast<uint64_t>(a) * b));
    break;
  case rsim::Insn::MULH:
    setReg(d.rd, static_cast<uint32_t>(
                      (static_cast<int64_t>(static_cast<int32_t>(a)) *
                       static_cast<int64_t>(static_cast<int32_t>(b))) >>
                      32));
    break;
  case rsim::Insn::MULHSU:
    setReg(d.rd, static_cast<uint32_t>(
                      (static_cast<int64_t>(static_cast<int32_t>(a)) *
                       static_cast<int64_t>(static_cast<uint64_t>(b))) >>
                      32));
    break;
  case rsim::Insn::MULHU:
    setReg(d.rd, static_cast<uint32_t>(
                      (static_cast<uint64_t>(a) * static_cast<uint64_t>(b)) >>
                      32));
    break;
  case rsim::Insn::DIV: {
    const int32_t sa = static_cast<int32_t>(a);
    const int32_t sb = static_cast<int32_t>(b);
    constexpr int32_t kMin = std::numeric_limits<int32_t>::min();
    if (sb == 0)
      setReg(d.rd, 0xffffffffu);
    else if (sa == kMin && sb == -1)
      setReg(d.rd, static_cast<uint32_t>(kMin));
    else
      setReg(d.rd, static_cast<uint32_t>(sa / sb));
    break;
  }
  case rsim::Insn::DIVU:
    setReg(d.rd, b == 0 ? 0xffffffffu : a / b);
    break;
  case rsim::Insn::REM: {
    const int32_t sa = static_cast<int32_t>(a);
    const int32_t sb = static_cast<int32_t>(b);
    constexpr int32_t kMin = std::numeric_limits<int32_t>::min();
    if (sb == 0)
      setReg(d.rd, a);
    else if (sa == kMin && sb == -1)
      setReg(d.rd, 0u);
    else
      setReg(d.rd, static_cast<uint32_t>(sa % sb));
    break;
  }
  case rsim::Insn::REMU:
    setReg(d.rd, b == 0 ? a : a % b);
    break;

  case rsim::Insn::FENCE:
  case rsim::Insn::FENCE_I:
    break; // single hart, no caches: nothing to order yet

  case rsim::Insn::ECALL:
  case rsim::Insn::EBREAK:
    // Chapter 01 shortcut: stop and let a0 speak.  Chapter 03 turns these
    // into EcallFromM/EcallFromU and Breakpoint traps.
    halted_ = true;
    exit_code_ = static_cast<int>(reg(10)); // a0, the kernel's return value
    break;

  default:
    throw Trap(TrapCause::IllegalInstruction, word, at,
               "instruction not implemented by this chapter");
  }

  pc = next;
  return !halted_;
}

} // namespace rmach
