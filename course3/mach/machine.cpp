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
#include <cstring>
#include <limits>
#include <string>

namespace rmach {

static_assert(sizeof(boot_seg) == 20, "boot_seg must be 20 bytes");
static_assert(sizeof(boot_info) == 16 + BOOT_MAX_SEGS * 20,
              "boot_info layout changed");
static_assert(sizeof(boot_info) * BOOT_MAX_PROCS <= 4096,
              "boot_info array must fit in its page");

namespace {

uint32_t sext8(uint8_t v) {
  return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(v)));
}
uint32_t sext16(uint16_t v) {
  return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(v)));
}

const char *csrText(uint16_t csr) {
  switch (csr) {
  case kCsrMstatus: return "mstatus";
  case kCsrMisa: return "misa";
  case kCsrMie: return "mie";
  case kCsrMtvec: return "mtvec";
  case kCsrMscratch: return "mscratch";
  case kCsrMepc: return "mepc";
  case kCsrMcause: return "mcause";
  case kCsrMtval: return "mtval";
  case kCsrMip: return "mip";
  case kCsrMhartid: return "mhartid";
  case kCsrMvendorid: return "mvendorid";
  case kCsrMarchid: return "marchid";
  case kCsrMimpid: return "mimpid";
  case kCsrMinstret: return "minstret";
  case kCsrSatp: return "satp";
  default: return nullptr;
  }
}

/// Course 2 formats an unknown CSR as "0x" + decimal digits, which turns
/// mhartid (0xf14) into "0x3860".  The machine prints its own CSRs in hex.
std::string formatForTrace(const rsim::Decoded &d) {
  if (d.fmt != rsim::Fmt::Csr || d.id == rsim::Insn::MRET)
    return rsim::format(d);
  std::string out = rsim::mnemonic(d.id);
  out += " ";
  out += rsim::registerName(d.rd);
  out += ", ";
  if (const char *name = csrText(d.csr))
    out += name;
  else {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "0x%03x", d.csr);
    out += buf;
  }
  out += ", ";
  if (rsim::isImmediateCsr(d.id))
    out += std::to_string(d.rs1);
  else
    out += rsim::registerName(d.rs1);
  return out;
}

} // namespace

void Machine::reset(const rsim::Image &image, Bus &bus) {
  bus_ = &bus;
  bus_->reset();
  bus_->ram().load(image);
  bus_->ram().map(kKernelStackTop - kKernelStackSize, kKernelStackSize);
  bus_->ram().map(kBootInfo, 4096);
  bus_->ram().map(kBootResult, 4096);
  bus_->ram().map(kUserBase, kUserSize);

  for (uint32_t &reg : x)
    reg = 0;
  pc = image.entry;
  x[2] = kKernelStackTop; // sp
  halted_ = false;
  executed_ = 0;
  exit_code_ = 0;
  timer_ticks_ = 0;
  ecall_count_ = 0;
  last_fetch_pa_ = 0;
  trace_ = nullptr;
  isa_ = kIsaDefault;
  for (uint32_t &word : csr_)
    word = 0;
  priv_ = Priv::M;
}

void Machine::setUser(uint32_t pid, const rsim::Image &user) {
  if (bus_ == nullptr)
    throw rsim::ElfError("setUser() called before reset()");
  if (pid >= BOOT_MAX_PROCS)
    throw rsim::ElfError("setUser() pid out of range");

  boot_info info = {};
  info.magic = BOOT_MAGIC;
  info.user_entry = user.entry;
  info.user_sp = BOOT_USER_SP;

  uint32_t cursor = BOOT_USER_IMAGE_BASE(pid);
  const uint32_t stack_phys = BOOT_USER_STACK_PHYS(pid);
  auto findPage = [&](uint32_t page_va) -> boot_seg * {
    for (uint32_t i = 0; i < info.nseg; ++i)
      if (info.seg[i].vaddr == page_va)
        return &info.seg[i];
    return nullptr;
  };

  for (const rsim::Segment &seg : user.segments) {
    if (seg.memsz == 0)
      continue;

    const uint32_t first = seg.vaddr & ~0xfffu;
    const uint32_t last = (seg.vaddr + seg.memsz - 1) & ~0xfffu;

    for (uint32_t page_va = first; page_va <= last; page_va += 4096) {
      boot_seg *page = findPage(page_va);
      if (page == nullptr) {
        if (info.nseg >= BOOT_MAX_SEGS)
          throw rsim::ElfError("too many user pages for boot_info");
        if (cursor + 4096 > stack_phys)
          throw rsim::ElfError("user image overlaps the reserved stack arena");

        page = &info.seg[info.nseg++];
        page->vaddr = page_va;
        page->phys = cursor;
        page->filesz = 4096;
        page->memsz = 4096;
        page->flags = 0;
        for (uint32_t i = 0; i < 4096; ++i)
          bus_->ram().write8(cursor + i, 0);
        cursor += 4096;
      }

      page->flags |= seg.flags;

      uint32_t copy_start = seg.vaddr > page_va ? seg.vaddr : page_va;
      uint32_t copy_end = seg.vaddr + seg.filesz;
      if (copy_end > page_va + 4096)
        copy_end = page_va + 4096;
      for (uint32_t va = copy_start; va < copy_end; ++va)
        bus_->ram().write8(page->phys + (va - page_va),
                           seg.data[va - seg.vaddr]);
    }
  }

  // The host and the guest are both little-endian; writing the struct byte by
  // byte keeps the C and C++ layouts honest (see the static_asserts above).
  const uint32_t info_addr = kBootInfo + pid * sizeof(boot_info);
  const auto *bytes = reinterpret_cast<const uint8_t *>(&info);
  for (size_t i = 0; i < sizeof(info); ++i)
    bus_->ram().write8(info_addr + static_cast<uint32_t>(i), bytes[i]);
  x[10] = kBootInfo; // a0: kmain(boot_info *)
}

uint32_t Machine::translate(uint32_t va, AccessType type,
                            uint32_t epc) const {
  // M-mode bypasses translation: the kernel runs on physical addresses even
  // after it writes satp.  U-mode uses Sv32 when MODE=1 and is bare when 0.
  if (priv_ == Priv::M)
    return va;
  if ((csr_[kCsrSatp] & kSatpMode) == 0)
    return va;
  return Mmu::translate(bus_->ram(), csr_[kCsrSatp], va, type, epc);
}

uint32_t Machine::fetchWord(uint32_t va) {
  const uint32_t pa = translate(va, AccessType::Fetch, va);
  last_fetch_pa_ = pa;
  try {
    return bus_->read32(pa);
  } catch (const rsim::MemoryError &err) {
    throw Trap(TrapCause::InstructionAccessFault, va, va, err.what());
  }
}

uint8_t Machine::load8(uint32_t addr, uint32_t epc) {
  const uint32_t pa = translate(addr, AccessType::Load, epc);
  try {
    return bus_->read8(pa);
  } catch (const rsim::MemoryError &err) {
    throw Trap(TrapCause::LoadAccessFault, addr, epc, err.what());
  }
}

uint16_t Machine::load16(uint32_t addr, uint32_t epc) {
  const uint32_t pa = translate(addr, AccessType::Load, epc);
  try {
    return bus_->read16(pa);
  } catch (const rsim::MemoryError &err) {
    throw Trap(TrapCause::LoadAccessFault, addr, epc, err.what());
  }
}

uint32_t Machine::load32(uint32_t addr, uint32_t epc) {
  const uint32_t pa = translate(addr, AccessType::Load, epc);
  try {
    return bus_->read32(pa);
  } catch (const rsim::MemoryError &err) {
    throw Trap(TrapCause::LoadAccessFault, addr, epc, err.what());
  }
}

void Machine::store8(uint32_t addr, uint8_t value, uint32_t epc) {
  const uint32_t pa = translate(addr, AccessType::Store, epc);
  try {
    bus_->write8(pa, value);
  } catch (const rsim::MemoryError &err) {
    throw Trap(TrapCause::StoreAccessFault, addr, epc, err.what());
  }
}

void Machine::store16(uint32_t addr, uint16_t value, uint32_t epc) {
  const uint32_t pa = translate(addr, AccessType::Store, epc);
  try {
    bus_->write16(pa, value);
  } catch (const rsim::MemoryError &err) {
    throw Trap(TrapCause::StoreAccessFault, addr, epc, err.what());
  }
}

void Machine::store32(uint32_t addr, uint32_t value, uint32_t epc) {
  const uint32_t pa = translate(addr, AccessType::Store, epc);
  try {
    bus_->write32(pa, value);
    updateTimer();
  } catch (const rsim::MemoryError &err) {
    throw Trap(TrapCause::StoreAccessFault, addr, epc, err.what());
  }
}

uint32_t Machine::readCsr(uint16_t csr) const {
  if (priv_ == Priv::U && isPrivilegedCsr(csr))
    throw Trap(TrapCause::IllegalInstruction, csr, pc,
               "privileged CSR read from U-mode");
  switch (csr) {
  case kCsrMstatus:
    return csr_[csr] & kMstatusMask;
  case kCsrMisa: // RV32, I and M; Zicsr is not a misa bit
    return (1u << 30) | (1u << 12) | (1u << 8);
  case kCsrMie:
    return csr_[csr] & kMTIE;
  case kCsrMip:
    return csr_[csr] & kMTIP;
  case kCsrMinstret:
    return static_cast<uint32_t>(executed_);
  case kCsrMhartid:
  case kCsrMvendorid:
  case kCsrMarchid:
  case kCsrMimpid:
    return 0;
  case kCsrMtvec:
  case kCsrMscratch:
  case kCsrMepc:
  case kCsrMcause:
  case kCsrMtval:
  case kCsrSatp:
    return csr_[csr];
  default:
    throw Trap(TrapCause::IllegalInstruction, csr, pc, "unknown CSR");
  }
}

void Machine::writeCsr(uint16_t csr, uint32_t value) {
  if (priv_ == Priv::U && isPrivilegedCsr(csr))
    throw Trap(TrapCause::IllegalInstruction, csr, pc,
               "privileged CSR write from U-mode");
  switch (csr) {
  case kCsrMstatus:
    csr_[csr] = value & kMstatusMask;
    return;
  case kCsrMie:
    csr_[csr] = value & kMTIE;
    return;
  case kCsrMip: // MTIP is owned by the CLINT (chapter 04)
    return;
  case kCsrMisa: // fixed WARL value: writes are ignored
    return;
  case kCsrMinstret:
    return;
  case kCsrMhartid:
  case kCsrMvendorid:
  case kCsrMarchid:
  case kCsrMimpid:
    throw Trap(TrapCause::IllegalInstruction, csr, pc, "read-only CSR write");
  case kCsrMtvec: // base is 4-byte aligned, mode is 0 or 1
    csr_[csr] = (value & ~3u) | (value & 1u);
    return;
  case kCsrMepc:
    csr_[csr] = value & ~3u;
    return;
  case kCsrMscratch:
  case kCsrMcause:
  case kCsrMtval:
    csr_[csr] = value;
    return;
  case kCsrSatp:
    csr_[csr] = value & kSatpMask;
    return;
  default:
    throw Trap(TrapCause::IllegalInstruction, csr, pc, "unknown CSR");
  }
}

bool Machine::step() {
  if (halted_)
    return false;
  if (takeInterrupt())
    return true; // the instruction at pc did not retire
  try {
    return executeOne();
  } catch (const Trap &trap) {
    return enterTrap(trap);
  }
}

bool Machine::timerInterruptPending() const {
  return (csr_[kCsrMip] & kMTIP) && (csr_[kCsrMie] & kMTIE) &&
         (csr_[kCsrMstatus] & kMstatusMIE);
}

void Machine::updateTimer() {
  if (bus_->clint().pending())
    csr_[kCsrMip] |= kMTIP;
  else
    csr_[kCsrMip] &= ~kMTIP;
}

bool Machine::takeInterrupt() {
  if ((csr_[kCsrMtvec] & ~3u) == 0)
    return false; // no handler yet
  if (!timerInterruptPending())
    return false;
  return enterTrap(kInterruptBit | 7u, 0, pc);
}

bool Machine::enterTrap(const Trap &trap) {
  return enterTrap(static_cast<uint32_t>(trap.cause), trap.tval, trap.epc);
}

bool Machine::enterTrap(uint32_t cause, uint32_t tval, uint32_t epc) {
  uint32_t base = csr_[kCsrMtvec] & ~3u;
  if (base == 0)
    throw; // no handler: the host has to see it

  // Vectored mtvec only affects interrupts; synchronous traps use the base.
  if ((cause & kInterruptBit) && (csr_[kCsrMtvec] & 1u))
    base += 4u * (cause & 0x7fffffffu);

  if ((cause & kInterruptBit) && (cause & 0x7fffffffu) == 7)
    ++timer_ticks_;
  if (cause == 8)
    ++ecall_count_;

  csr_[kCsrMepc] = epc;
  csr_[kCsrMcause] = cause;
  csr_[kCsrMtval] = tval;

  uint32_t ms = csr_[kCsrMstatus];
  ms &= ~(kMstatusMPIE | kMstatusMPP);
  if (csr_[kCsrMstatus] & kMstatusMIE)
    ms |= kMstatusMPIE;
  ms |= static_cast<uint32_t>(priv_) << 11;
  ms &= ~kMstatusMIE;
  csr_[kCsrMstatus] = ms;

  priv_ = Priv::M;
  pc = base;
  return true;
}

bool Machine::executeOne() {
  const uint32_t at = pc;
  if (at & 3u)
    throw Trap(TrapCause::InstructionAddressMisaligned, at, at,
               "pc is not 4-byte aligned");

  const uint32_t word = fetchWord(at);

  const rsim::Decoded d = rsim::decode(word);
  if (d.id == rsim::Insn::ILLEGAL)
    throw Trap(TrapCause::IllegalInstruction, word, at,
               "no implemented instruction matches");
  if ((isa_ & static_cast<uint32_t>(rsim::isaOf(d.id))) == 0)
    throw Trap(TrapCause::IllegalInstruction, word, at,
               "instruction not enabled by the ISA mask");
  ++executed_;
  bus_->clint().tick();
  updateTimer();

  if (trace_) {
    const char priv = priv_ == Priv::M ? 'M' : 'U';
    std::fprintf(trace_, "T %08" PRIx32 " %08" PRIx32 " %c %08" PRIx32
                         " %s\n",
                 at, last_fetch_pa_, priv, word,
                 formatForTrace(d).c_str());
  }

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

  case rsim::Insn::CSRRW:
  case rsim::Insn::CSRRWI:
  case rsim::Insn::CSRRS:
  case rsim::Insn::CSRRSI:
  case rsim::Insn::CSRRC:
  case rsim::Insn::CSRRCI: {
    const uint32_t old = readCsr(d.csr);
    const uint32_t src = rsim::isImmediateCsr(d.id) ? d.rs1 : a;
    switch (d.id) {
    case rsim::Insn::CSRRW:
    case rsim::Insn::CSRRWI:
      writeCsr(d.csr, src);
      break;
    case rsim::Insn::CSRRS:
    case rsim::Insn::CSRRSI:
      if (src != 0) // csrr with x0 reads without writing
        writeCsr(d.csr, old | src);
      break;
    default:
      if (src != 0)
        writeCsr(d.csr, old & ~src);
      break;
    }
    setReg(d.rd, old);
    break;
  }

  case rsim::Insn::ECALL:
    // Bootstrap shortcut: without mtvec there is nobody to handle the ecall,
    // so a0 becomes the host exit code.  Once a handler is installed this is
    // a real EcallFromM/EcallFromU trap.
    if ((csr_[kCsrMtvec] & ~3u) == 0) {
      halted_ = true;
      exit_code_ = static_cast<int>(reg(10));
      break;
    }
    throw Trap(priv_ == Priv::U ? TrapCause::EcallFromU : TrapCause::EcallFromM,
               0, at, "ecall");
  case rsim::Insn::EBREAK:
    if ((csr_[kCsrMtvec] & ~3u) == 0) {
      halted_ = true;
      exit_code_ = static_cast<int>(reg(10));
      break;
    }
    throw Trap(TrapCause::Breakpoint, at, at, "ebreak");

  case rsim::Insn::MRET: {
    const uint32_t ms = csr_[kCsrMstatus];
    const uint32_t mpp = (ms >> 11) & 3u;
    if (mpp != 0 && mpp != 3)
      throw Trap(TrapCause::IllegalInstruction, word, at,
                 "mret with an unsupported MPP");
    priv_ = mpp == 3 ? Priv::M : Priv::U;
    uint32_t next_ms = ms & ~kMstatusMIE;
    if (ms & kMstatusMPIE)
      next_ms |= kMstatusMIE;
    next_ms |= kMstatusMPIE;
    next_ms &= ~kMstatusMPP; // the least-privileged level is U
    csr_[kCsrMstatus] = next_ms;
    next = csr_[kCsrMepc];
    break;
  }

  default:
    throw Trap(TrapCause::IllegalInstruction, word, at,
               "instruction not implemented by this chapter");
  }

  pc = next;
  if (bus_->finished()) {
    halted_ = true;
    exit_code_ = bus_->exitCode();
  }
  return !halted_;
}

} // namespace rmach
