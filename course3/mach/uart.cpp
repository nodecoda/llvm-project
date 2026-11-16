//===-- uart.cpp - byte-level console, chapter 05 ---------------------------===//

#include "uart.h"

namespace rmach {

int Uart::inputByte() {
  if (peek_ == -2)
    peek_ = in_ ? std::fgetc(in_) : -1;
  return peek_;
}

uint8_t Uart::read(uint32_t addr) {
  switch (addr - kBase) {
  case 0: { // RBR
    const int c = inputByte();
    if (c < 0)
      return 0;
    peek_ = -2;
    return static_cast<uint8_t>(c);
  }
  case 1:
    return ier_;
  case 2:
    return 0x01; // no interrupt pending
  case 3:
    return lcr_;
  case 4:
    return mcr_;
  case 5:
    return static_cast<uint8_t>(kLsrThre | (inputByte() >= 0 ? kLsrDr : 0));
  case 6:
    return 0;
  default:
    return scr_;
  }
}

void Uart::write(uint32_t addr, uint8_t value) {
  switch (addr - kBase) {
  case 0: // THR
    std::fputc(value, stdout);
    return;
  case 1:
    ier_ = value;
    return;
  case 2: // FCR
    return;
  case 3:
    lcr_ = value;
    return;
  case 4:
    mcr_ = value;
    return;
  default:
    scr_ = value;
    return;
  }
}

} // namespace rmach
