//===-- demo.c - a freestanding RVMin program for chapter 12 ----*- C -*-===//
//
// Compiled with clang (RV32I, ilp32), lowered by `llc -march=rvmin`, linked
// against rvmin-rt.S + start.S, and executed under QEMU user mode.
//
// main() returns 0 when every check passes, otherwise the number of the first
// failing check, which becomes the process exit code.
//
//===----------------------------------------------------------------------===//

int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

unsigned crc32(const unsigned char *data, unsigned len) {
  unsigned crc = 0xFFFFFFFFu;
  for (unsigned i = 0; i < len; i++) {
    crc ^= data[i];
    for (int k = 0; k < 8; k++)
      crc = (crc >> 1) ^ (0xEDB88320u & (-(int)(crc & 1)));
  }
  return ~crc;
}

static int narrow(signed char c) { return c; }

int main(void) {
  if (fib(10) != 55)                       // call, recursion, stack frames
    return 1;
  if (1000 * 37 != 37000)                  // __mulsi3
    return 2;
  if (37000u / 37u != 1000u)               // __udivsi3
    return 3;
  if (-37000 / 37 != -1000)                // __divsi3
    return 4;
  if (-37 % 10 != -7)                      // __modsi3
    return 5;
  if (37000u % 37u != 0u)                  // __umodsi3
    return 6;
  if ((1LL << 40) != 1099511627776LL)      // __ashldi3
    return 7;
  if ((-1024LL >> 3) != -128LL)            // __ashrdi3
    return 8;
  if ((0xFFFFFFFFu >> 4) != 0x0FFFFFFFu)   // srli
    return 9;
  if (crc32((const unsigned char *)"123456789", 9) != 0xCBF43926u)
    return 10;                             // loops, globals, bit tricks
  if (narrow((signed char)200) != -56)     // sign_extend_inreg
    return 11;
  return 0;
}
