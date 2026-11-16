//===-- cpu.h - register file, reset state and the step loop, ch 02/03 -*- C++ -*-===//
//
// Chapter 02 gave the CPU its architectural state and the user stack.
// Chapter 03 adds step(): fetch, decode, execute, update pc.
//
//===----------------------------------------------------------------------===//

#ifndef RSIM_CPU_H
#define RSIM_CPU_H

#include "elf.h"
#include "memory.h"
#include "trap.h"

#include <cstdint>
#include <cstdio>

namespace rsim {

class Cpu {
public:
  // The user-mode stack lives in the last MiB of the 32-bit address space,
  // like QEMU user mode.  QEMU sets sp itself; an emulator has to do it.
  static constexpr uint32_t kStackBase = 0x7ff00000;
  static constexpr uint32_t kStackSize = 0x00100000; // 1 MiB
  static constexpr uint32_t kStackTop = 0x7ffff000;  // 16-byte aligned

  // x0 is hard-wired zero; x2 is sp, x1 is ra.
  uint32_t x[32] = {};
  uint32_t pc = 0;

  void reset(const Image &image, Memory &memory);
  void attach(Memory &memory) { memory_ = &memory; }

  /// Chapter 09: which extensions may execute.  The default is everything
  /// the emulator implements; the CLI narrows it with --isa.
  static constexpr uint32_t kIsaAll = 0xffffffffu;
  void setIsa(uint32_t mask) { isa_ = mask; }
  uint32_t isa() const { return isa_; }

  Memory &memory() {
    if (!memory_)
      throw Trap(TrapCause::InstructionAccessFault, 0, pc,
                 "CPU is not attached to memory");
    return *memory_;
  }

  /// Execute one instruction.  Returns false once the program has halted
  /// (ecall, ebreak until chapter 05/08 give them meaning).
  bool step();

  bool halted() const { return halted_; }
  uint64_t executed() const { return executed_; }
  int exitCode() const { return exit_code_; }

  // Chapter 08: the minimum machine-mode CSR set.
  uint32_t mtvec() const { return mtvec_; }
  uint32_t mepc() const { return mepc_; }
  uint32_t mcause() const { return mcause_; }
  uint32_t mtval() const { return mtval_; }
  uint32_t readCsr(uint16_t csr) const;
  void writeCsr(uint16_t csr, uint32_t value);

  /// Chapter 06: print one line per executed instruction to `file`.
  void traceTo(std::FILE *file) { trace_ = file; }

  /// x0 discards writes, wherever they come from.
  void setReg(unsigned reg, uint32_t value) {
    if (reg != 0)
      x[reg & 31] = value;
  }
  uint32_t reg(unsigned r) const { return x[r & 31]; }

  uint32_t sp() const { return x[2]; }
  static bool inStackRange(uint64_t addr, uint64_t size) {
    return addr >= kStackBase &&
           addr + size <= static_cast<uint64_t>(kStackBase) + kStackSize;
  }

private:
  /// One instruction without the trap wrapper: `step()` turns a Trap into an
  /// entry into the handler when mtvec is programmed.
  bool executeOne();

  /// Handle one ecall.  Returns true when the program continues.
  bool ecall(uint32_t at);

  // Every access goes through these so that an unusable address becomes a
  // trap with the *faulting* pc, never a silently zeroed register.
  uint8_t load8(uint32_t addr, uint32_t epc);
  uint16_t load16(uint32_t addr, uint32_t epc);
  uint32_t load32(uint32_t addr, uint32_t epc);
  void store8(uint32_t addr, uint8_t value, uint32_t epc);
  void store16(uint32_t addr, uint16_t value, uint32_t epc);
  void store32(uint32_t addr, uint32_t value, uint32_t epc);

  Memory *memory_ = nullptr;
  bool halted_ = false;
  uint64_t executed_ = 0;
  int exit_code_ = 0;
  std::FILE *trace_ = nullptr;
  uint32_t mtvec_ = 0;
  uint32_t mepc_ = 0;
  uint32_t mcause_ = 0;
  uint32_t mtval_ = 0;
  uint32_t isa_ = kIsaAll;
};

} // namespace rsim

#endif // RSIM_CPU_H
