//===-- float.c - F runtime checks for chapter 42 ------------*- C -*-===//
//
// Freestanding: no libc, no string formatting.  main() returns 0 when every
// check passes, otherwise the number of the first failing check.  The
// volatile globals and the noinline helpers stop clang from folding the
// arithmetic or the conversions away, so the program really executes the F
// instructions emitted by the backend.
//
// rvmin-run.sh builds this twice, once with F only (rv32if/ilp32f) and once
// with D enabled (rv32id/ilp32d, where D implies F).
//
//===----------------------------------------------------------------------===//

volatile float f1 = 1.5f, f2 = 2.25f;
volatile int ci = 7;
volatile int isink;
volatile float fsink;

// Nine floats: the ninth arrives on the stack.
__attribute__((noinline))
static float add9f(float a, float b, float c, float d, float e, float f,
                   float g, float h, float i) {
  return a + b + c + d + e + f + g + h + i;
}

// The helper boundary makes `x` opaque, so the conversions cannot be folded
// using facts from the caller's earlier comparisons.
__attribute__((noinline)) static int f2i(float x) {
  isink = (int)x;                  // fcvt.w.s rtz
  return isink;
}

__attribute__((noinline)) static float i2f(int x) {
  fsink = (float)x;                // fcvt.s.w
  return fsink;
}

int main(void) {
  if (f1 + f2 != 3.75f)                    // fadd.s + feq.s
    return 1;
  if (f2i(f1) != 1)                        // fcvt.w.s rtz
    return 2;
  if (i2f(ci) != 7.0f)                     // fcvt.s.w
    return 3;
  if (!(f1 < f2))                          // flt.s
    return 4;
  if (f1 * f2 != 3.375f)                   // fmul.s
    return 5;

  if (add9f(1, 2, 3, 4, 5, 6, 7, 8, 9) != 45.0f) // stack argument
    return 6;

  if (f1 * 1.5f + 2.0f != 4.25f)           // constants via fmv.w.x
    return 7;

  if ((f1 > f2 ? f1 : f2) != 2.25f)        // f32 SELECT via GPR mask
    return 8;

  return 0;
}
