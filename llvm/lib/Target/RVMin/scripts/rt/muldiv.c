//===-- muldiv.c - constant multiply/divide under QEMU, chapter 19 -*- C -*-===//
//
// Compiled with clang at -O2, lowered by `llc -march=rvmin`, and run under
// QEMU.  The expected values are written as literals on purpose: computing
// them with `/` or `%` would exercise the same code path and prove nothing.
//
// main() returns 0 when every check passes, otherwise the number of the first
// failing check, which becomes the process exit code.
//
//===----------------------------------------------------------------------===//

int mul3(int x) { return x * 3; }
int mul5(int x) { return x * 5; }
int mul7(int x) { return x * 7; }
int mul10(int x) { return x * 10; }
int mul64(int x) { return x * 64; }
int div7(int x) { return x / 7; }
unsigned udiv7(unsigned x) { return x / 7; }
int mod10(int x) { return x % 10; }

volatile int v = -37;
volatile unsigned uv = 0xFFFFFFFFu;

int main(void) {
  // Negative operands: the shift/add forms must still wrap like C.
  if (mul3(v) != -111)
    return 1;
  if (mul5(v) != -185)
    return 2;
  if (mul7(v) != -259)
    return 3;
  if (mul10(v) != -370)
    return 4;
  if (mul64(v) != -2368)
    return 5;

  // C division truncates toward zero and the remainder follows the dividend.
  if (div7(v) != -5)
    return 6;
  if (mod10(v) != -7)
    return 7;

  v = 37;
  if (mul3(v) != 111)
    return 8;
  if (div7(v) != 5)
    return 9;
  if (mod10(v) != 7)
    return 10;

  // INT_MIN: the magic-number sequence has a special case for it.
  v = (int)0x80000000u;
  if (div7(v) != -306783378)
    return 11;
  if (mod10(v) != -8)
    return 12;

  if (udiv7(uv) != 613566756u)
    return 13;
  uv = 0u;
  if (udiv7(uv) != 0u)
    return 14;

  return 0;
}
