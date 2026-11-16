//===-- cpu.cpp - register file and reset state, chapter 02 ----------------===//

#include "cpu.h"

namespace rsim {

void Cpu::reset(const Image &image, Memory &memory) {
  memory_ = &memory;
  memory.load(image);
  memory.map(kStackBase, kStackSize);

  for (uint32_t &reg : x)
    reg = 0;
  pc = image.entry;
  x[2] = kStackTop; // sp
}

} // namespace rsim
