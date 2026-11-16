//===-- memory.h - sparse 32-bit address space, chapter 01 ------*- C++ -*-===//
//
// Pages are allocated on demand, so a 4 GiB address space costs only the
// pages the program actually touches.  An access to an unmapped byte is a
// hard error: the emulator never invents memory.
//
//===----------------------------------------------------------------------===//

#ifndef RSIM_MEMORY_H
#define RSIM_MEMORY_H

#include "elf.h"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace rsim {

struct MemoryError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

class Memory {
public:
  static constexpr uint32_t kPageShift = 12;
  static constexpr uint32_t kPageSize = 1u << kPageShift;

  void map(uint64_t addr, uint64_t size);
  bool isMapped(uint64_t addr, uint64_t size) const;
  void load(const Image &image);

  std::vector<uint8_t> readBytes(uint64_t addr, uint64_t size) const;

  uint8_t read8(uint32_t addr) const;
  uint16_t read16(uint32_t addr) const;
  uint32_t read32(uint32_t addr) const;

  void write8(uint32_t addr, uint8_t value);
  void write16(uint32_t addr, uint16_t value);
  void write32(uint32_t addr, uint32_t value);

  size_t mappedPages() const { return pages_.size(); }

private:
  std::unordered_map<uint32_t, std::array<uint8_t, kPageSize>> pages_;

  static uint32_t pageBase(uint32_t addr) { return addr & ~(kPageSize - 1); }
  uint8_t *pageFor(uint32_t addr);
  const uint8_t *pageFor(uint32_t addr) const;
};

} // namespace rsim

#endif // RSIM_MEMORY_H
