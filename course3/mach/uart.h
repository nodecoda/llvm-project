//===-- uart.h - the 16550 subset, chapter 05 --------------------*- C++ -*-===//
//
// Only the registers a polling console needs: THR/RBR, IER, IIR/FCR, LCR and
// LSR.  Writes to THR go to stdout; RBR comes from --input.  Interrupts are
// wired in chapter 09.
//
//===----------------------------------------------------------------------===//

#ifndef RMACH_UART_H
#define RMACH_UART_H

#include <cstdint>
#include <cstdio>

namespace rmach {

class Uart {
public:
  static constexpr uint32_t kBase = 0x10000000;
  static constexpr uint32_t kWindow = 0x100;

  static constexpr uint8_t kLsrDr = 0x01;
  static constexpr uint8_t kLsrThre = 0x20;

  void reset() {
    // in_ survives reset on purpose: main wires --input before Machine::reset.
    ier_ = 0;
    lcr_ = 0;
    mcr_ = 0;
    scr_ = 0;
    peek_ = -2;
  }

  void setInput(std::FILE *in) { in_ = in; }

  bool contains(uint32_t addr) const { return addr - kBase < kWindow; }

  uint8_t read(uint32_t addr);
  void write(uint32_t addr, uint8_t value);

private:
  int inputByte(); // -1 at EOF

  std::FILE *in_ = nullptr;
  int peek_ = -2; // -2 = not looked yet, -1 = EOF
  uint8_t ier_ = 0;
  uint8_t lcr_ = 0;
  uint8_t mcr_ = 0;
  uint8_t scr_ = 0;
};

} // namespace rmach

#endif // RMACH_UART_H
