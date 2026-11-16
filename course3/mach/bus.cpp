//===-- bus.cpp - RAM/CLINT/UART/test-finisher decode, chapter 05 -----------===//

#include "bus.h"

#include <cstdio>

namespace rmach {

namespace {
[[noreturn]] void badWidth(uint32_t addr, const char *what) {
  char buf[96];
  std::snprintf(buf, sizeof(buf), "%s access at 0x%08x", what, addr);
  throw rsim::MemoryError(buf);
}
} // namespace

void Bus::reset() {
  clint_.reset();
  uart_.reset();
  finished_ = false;
  exit_code_ = 0;
}

uint32_t Bus::read32(uint32_t addr) {
  if (isTest(addr))
    return 0; // the test word is write-only
  if (clint_.contains(addr)) {
    if (addr == Clint::kMtime)
      return static_cast<uint32_t>(clint_.mtime());
    if (addr == Clint::kMtime + 4)
      return static_cast<uint32_t>(clint_.mtime() >> 32);
    if (addr == Clint::kMtimecmp)
      return static_cast<uint32_t>(clint_.mtimecmp());
    if (addr == Clint::kMtimecmp + 4)
      return static_cast<uint32_t>(clint_.mtimecmp() >> 32);
    badWidth(addr, "unmapped CLINT");
  }
  if (uart_.contains(addr))
    badWidth(addr, "32-bit UART"); // the 16550 subset is byte-only
  return ram_.read32(addr);
}

uint8_t Bus::read8(uint32_t addr) {
  if (uart_.contains(addr))
    return uart_.read(addr);
  if (clint_.contains(addr) || isTest(addr))
    badWidth(addr, "8-bit device");
  return ram_.read8(addr);
}

uint16_t Bus::read16(uint32_t addr) {
  if (uart_.contains(addr))
    badWidth(addr, "16-bit UART");
  if (clint_.contains(addr) || isTest(addr))
    badWidth(addr, "16-bit device");
  return ram_.read16(addr);
}

void Bus::write32(uint32_t addr, uint32_t value) {
  if (isTest(addr)) {
    const uint32_t code = value & 0xffffu;
    finished_ = true;
    if (code == 0x7777u)
      exit_code_ = 1; // the SiFive failure signature
    else
      exit_code_ = static_cast<int>(value >> 16); // 0x5555 keeps the code
    return;
  }
  if (clint_.contains(addr)) {
    if (addr == Clint::kMtimecmp || addr == Clint::kMtimecmp + 4) {
      clint_.writeMtimecmp(value, addr == Clint::kMtimecmp + 4);
      return;
    }
    badWidth(addr, "write to read-only CLINT");
  }
  if (uart_.contains(addr))
    badWidth(addr, "32-bit UART");
  ram_.write32(addr, value);
}

void Bus::write8(uint32_t addr, uint8_t value) {
  if (uart_.contains(addr)) {
    uart_.write(addr, value);
    return;
  }
  if (clint_.contains(addr) || isTest(addr))
    badWidth(addr, "8-bit device write");
  ram_.write8(addr, value);
}

void Bus::write16(uint32_t addr, uint16_t value) {
  if (uart_.contains(addr))
    badWidth(addr, "16-bit UART");
  if (clint_.contains(addr) || isTest(addr))
    badWidth(addr, "16-bit device write");
  ram_.write16(addr, value);
}

} // namespace rmach
