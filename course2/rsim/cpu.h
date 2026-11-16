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

#include <cstdint>
#include <stdexcept>
#include <string>

namespace rsim {

/// A trap the guest cannot ignore.  Chapter 08 will add mtvec/mepc/mcause;
/// for now a trap is just "stop with an explanation".
struct Trap : std::runtime_error {
  using std::runtime_error::runtime_error;
};

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

  Memory &memory() {
    if (!memory_)
      throw Trap("CPU is not attached to memory");
    return *memory_;
  }

  /// Execute one instruction.  Returns false once the program has halted
  /// (ecall, ebreak until chapter 05/08 give them meaning).
  bool step();

  bool halted() const { return halted_; }
  uint64_t executed() const { return executed_; }

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
  Memory *memory_ = nullptr;
  bool halted_ = false;
  uint64_t executed_ = 0;
};

} // namespace rsim

#endif // RSIM_CPU_H
