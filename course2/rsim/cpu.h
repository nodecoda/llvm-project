//===-- cpu.h - register file and reset state, chapter 02 -------*- C++ -*-===//
//
// Chapter 02 only needs the architectural state and the stack.  Instructions
// arrive in chapter 03.
//
//===----------------------------------------------------------------------===//

#ifndef RSIM_CPU_H
#define RSIM_CPU_H

#include "elf.h"
#include "memory.h"

#include <cstdint>

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

  Memory &memory() {
    if (!memory_)
      throw MemoryError("CPU is not attached to memory");
    return *memory_;
  }

  uint32_t sp() const { return x[2]; }
  static bool inStackRange(uint64_t addr, uint64_t size) {
    return addr >= kStackBase &&
           addr + size <= static_cast<uint64_t>(kStackBase) + kStackSize;
  }

private:
  Memory *memory_ = nullptr;
};

} // namespace rsim

#endif // RSIM_CPU_H
