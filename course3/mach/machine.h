//===-- machine.h - registers, reset and the step loop, chapter 01 -*- C++ -*-===//
//
// Chapter 01 is deliberately unprivileged: the kernel boots in M-mode-ish
// state, fetches from physical memory and halts on ebreak.  Chapters 02-06
// add the CSR file, the privilege transitions, the timer, the bus and Sv32.
//
//===----------------------------------------------------------------------===//

#ifndef RMACH_MACHINE_H
#define RMACH_MACHINE_H

#include "bus.h"
#include "csr.h"
#include "elf.h"
#include "decode.h"
#include "mmu.h"
#include "trap.h"

#include <cstdint>
#include <cstdio>

namespace rmach {

class Machine {
public:
  // The kernel image is linked at 0x80000000; its stack lives just above it.
  static constexpr uint32_t kKernelStackTop = 0x80100000;
  static constexpr uint32_t kKernelStackSize = 0x00004000; // 16 KiB

  // Physical arena for user images and their page tables.  Chapter 06 uses a
  // fixed scratch page; chapter 07 copies each PT_LOAD segment here.
  static constexpr uint32_t kUserBase = 0x80400000;
  static constexpr uint32_t kUserSize = 0x00010000; // 64 KiB

  static constexpr uint32_t kIsaDefault =
      static_cast<uint32_t>(rsim::Isa::RV32I) |
      static_cast<uint32_t>(rsim::Isa::RV32M);

  uint32_t x[32] = {};
  uint32_t pc = 0;

  void reset(const rsim::Image &image, Bus &bus);
  void setIsa(uint32_t mask) { isa_ = mask; }

  // Chapter 02: the CSR file and the current privilege level.  The selftest
  // drives these directly; the CPU changes them through instructions.
  uint32_t readCsr(uint16_t csr) const;
  void writeCsr(uint16_t csr, uint32_t value);
  void setPrivilege(Priv p) { priv_ = p; }
  Priv privilege() const { return priv_; }
  /// Debug view of satp that ignores privilege, for --dump-pagetable.
  uint32_t satp() const { return csr_[kCsrSatp]; }

  /// Execute one instruction.  Returns false once the machine has halted.
  bool step();

  bool halted() const { return halted_; }
  uint64_t executed() const { return executed_; }
  int exitCode() const { return exit_code_; }

  void traceTo(std::FILE *file) { trace_ = file; }

  /// x0 discards writes, wherever they come from.
  void setReg(unsigned reg, uint32_t value) {
    if (reg != 0)
      x[reg & 31] = value;
  }
  uint32_t reg(unsigned r) const { return x[r & 31]; }

private:
  bool executeOne();
  bool enterTrap(const Trap &trap);
  bool enterTrap(uint32_t cause, uint32_t tval, uint32_t epc);
  bool takeInterrupt();
  bool timerInterruptPending() const;
  void updateTimer();

  // Every access goes through these so that an unusable address becomes a
  // trap with the *faulting* pc, never a silently zeroed register.  The
  // translate() step is identity in M-mode and Sv32 in U-mode (chapter 06).
  uint32_t translate(uint32_t va, AccessType type, uint32_t epc) const;
  uint32_t fetchWord(uint32_t va);
  uint8_t load8(uint32_t addr, uint32_t epc);
  uint16_t load16(uint32_t addr, uint32_t epc);
  uint32_t load32(uint32_t addr, uint32_t epc);
  void store8(uint32_t addr, uint8_t value, uint32_t epc);
  void store16(uint32_t addr, uint16_t value, uint32_t epc);
  void store32(uint32_t addr, uint32_t value, uint32_t epc);

  Bus *bus_ = nullptr;
  bool halted_ = false;
  uint64_t executed_ = 0;
  int exit_code_ = 0;
  std::FILE *trace_ = nullptr;
  uint32_t isa_ = kIsaDefault;
  uint32_t csr_[4096] = {};
  Priv priv_ = Priv::M;
};

} // namespace rmach

#endif // RMACH_MACHINE_H
