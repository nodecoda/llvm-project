//===-- memory.cpp - sparse 32-bit address space, chapter 01 ---------------===//

#include "memory.h"

#include <cstdio>
#include <cstring>

namespace rsim {

void Memory::map(uint64_t addr, uint64_t size) {
  if (size == 0)
    return;
  const uint64_t end = addr + size;
  if (end > (uint64_t{1} << 32))
    throw MemoryError("mapping overflows the 32-bit address space");
  for (uint64_t base = addr & ~uint64_t{kPageSize - 1}; base < end;
       base += kPageSize)
    pages_.try_emplace(static_cast<uint32_t>(base));
}

bool Memory::isMapped(uint64_t addr, uint64_t size) const {
  if (size == 0)
    return true;
  const uint64_t end = addr + size;
  if (end > (uint64_t{1} << 32))
    return false;
  for (uint64_t base = addr & ~uint64_t{kPageSize - 1}; base < end;
       base += kPageSize) {
    if (pages_.find(static_cast<uint32_t>(base)) == pages_.end())
      return false;
  }
  return true;
}

void Memory::load(const Image &image) {
  for (const Segment &seg : image.segments) {
    map(seg.vaddr, seg.memsz);
    for (uint32_t i = 0; i < seg.filesz; ++i)
      write8(seg.vaddr + i, seg.data[i]);
  }
}

namespace {
std::string addrString(uint32_t addr) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "0x%08x", addr);
  return buf;
}
} // namespace

uint8_t *Memory::pageFor(uint32_t addr) {
  auto it = pages_.find(pageBase(addr));
  if (it == pages_.end())
    throw MemoryError("unmapped access at " + addrString(addr));
  return it->second.data();
}

const uint8_t *Memory::pageFor(uint32_t addr) const {
  auto it = pages_.find(pageBase(addr));
  if (it == pages_.end())
    throw MemoryError("unmapped access at " + addrString(addr));
  return it->second.data();
}

std::vector<uint8_t> Memory::readBytes(uint64_t addr, uint64_t size) const {
  if (!isMapped(addr, size))
    throw MemoryError("unmapped access at " + addrString(uint32_t(addr)));
  std::vector<uint8_t> bytes(size);
  for (uint64_t i = 0; i < size; ++i)
    bytes[i] = read8(uint32_t(addr + i));
  return bytes;
}

uint8_t Memory::read8(uint32_t addr) const {
  return pageFor(addr)[addr & (kPageSize - 1)];
}

uint16_t Memory::read16(uint32_t addr) const {
  return static_cast<uint16_t>(read8(addr) | (read8(addr + 1) << 8));
}

uint32_t Memory::read32(uint32_t addr) const {
  return static_cast<uint32_t>(read8(addr)) |
         (static_cast<uint32_t>(read8(addr + 1)) << 8) |
         (static_cast<uint32_t>(read8(addr + 2)) << 16) |
         (static_cast<uint32_t>(read8(addr + 3)) << 24);
}

void Memory::write8(uint32_t addr, uint8_t value) {
  pageFor(addr)[addr & (kPageSize - 1)] = value;
}

void Memory::write16(uint32_t addr, uint16_t value) {
  write8(addr, static_cast<uint8_t>(value));
  write8(addr + 1, static_cast<uint8_t>(value >> 8));
}

void Memory::write32(uint32_t addr, uint32_t value) {
  write8(addr, static_cast<uint8_t>(value));
  write8(addr + 1, static_cast<uint8_t>(value >> 8));
  write8(addr + 2, static_cast<uint8_t>(value >> 16));
  write8(addr + 3, static_cast<uint8_t>(value >> 24));
}

} // namespace rsim
