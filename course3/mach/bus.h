//===-- bus.h - address decode for RAM, CLINT, UART and the finisher, ch 05 -===//
//
// One place decides which device owns an address, so the CPU never grows a
// second address decoder.  The test finisher is the SiFive-compatible word at
// 0x00100000: 0x5555 means "pass", 0x7777 means "fail", anything else carries
// its exit code in the upper half.
//
//===----------------------------------------------------------------------===//

#ifndef RMACH_BUS_H
#define RMACH_BUS_H

#include "clint.h"
#include "uart.h"

#include "memory.h"

#include <cstdint>

namespace rmach {

class Bus {
public:
  static constexpr uint32_t kTestBase = 0x00100000;

  void reset();

  rsim::Memory &ram() { return ram_; }
  Clint &clint() { return clint_; }
  Uart &uart() { return uart_; }

  uint8_t read8(uint32_t addr);
  uint16_t read16(uint32_t addr);
  uint32_t read32(uint32_t addr);
  void write8(uint32_t addr, uint8_t value);
  void write16(uint32_t addr, uint16_t value);
  void write32(uint32_t addr, uint32_t value);

  bool finished() const { return finished_; }
  int exitCode() const { return exit_code_; }

private:
  bool isTest(uint32_t addr) const { return addr - kTestBase < 0x1000u; }

  rsim::Memory ram_;
  Clint clint_;
  Uart uart_;
  bool finished_ = false;
  int exit_code_ = 0;
};

} // namespace rmach

#endif // RMACH_BUS_H
