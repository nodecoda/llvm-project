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

// The operands are volatile so that the compiler cannot fold the arithmetic
// away: chapter 12 needs real calls to __mulsi3/__divsi3/..., and chapter 13
// needs real `mul`/`div`/`rem` instructions instead of them.
volatile int vmul_a = 1000, vmul_b = 37;
volatile unsigned vdiv_a = 37000u, vdiv_b = 37u;
volatile int vsdiv_a = -37000, vsdiv_b = 37;
volatile int vmod_a = -37, vmod_b = 10;
volatile unsigned vumod_a = 37000u, vumod_b = 37u;
volatile long long vshift = 1LL;
volatile long long vneg = -1024LL;
volatile int vcount = 40;

int main(void) {
  if (fib(10) != 55)                       // call, recursion, stack frames
    return 1;
  if (vmul_a * vmul_b != 37000)            // __mulsi3 / mul
    return 2;
  if (vdiv_a / vdiv_b != 1000u)            // __udivsi3 / divu
    return 3;
  if (vsdiv_a / vsdiv_b != -1000)          // __divsi3 / div
    return 4;
  if (vmod_a % vmod_b != -7)               // __modsi3 / rem
    return 5;
  if (vumod_a % vumod_b != 0u)             // __umodsi3 / remu
    return 6;
  if ((vshift << vcount) != 1099511627776LL) // __ashldi3 (dynamic count)
    return 7;
  if ((vneg >> 3) != -128LL)               // __ashrdi3
    return 8;
  if ((0xFFFFFFFFu >> 4) != 0x0FFFFFFFu)   // srli
    return 9;
  if (crc32((const unsigned char *)"123456789", 9) != 0xCBF43926u)
    return 10;                             // loops, globals, bit tricks
  if (narrow((signed char)200) != -56)     // sign_extend_inreg
    return 11;
  return 0;
}
