//===-- double.c - D runtime checks for chapter 42 -----------*- C -*-===//
//
// D implies F, so this program needs the full -march=rv32id -mabi=ilp32d
// toolchain contract.  main() returns 0 when every check passes, otherwise
// the number of the first failing check.
//
//===----------------------------------------------------------------------===//

volatile double d1 = 1.5, d2 = 2.25;
volatile int ci = 7;
volatile int isink;
volatile double dsink;

// Nine doubles: the ninth arrives on the stack, and the sum forces the
// callee to keep values live across the calls it makes.
__attribute__((noinline))
static double add9(double a, double b, double c, double d, double e,
                   double f, double g, double h, double i) {
  return a + b + c + d + e + f + g + h + i;
}

__attribute__((noinline)) static int d2i(double x) {
  isink = (int)x;                  // fcvt.w.d rtz
  return isink;
}

__attribute__((noinline)) static double i2d(int x) {
  dsink = (double)x;               // fcvt.d.w
  return dsink;
}

// (double)(float)x, kept opaque so both width conversions really happen.
__attribute__((noinline)) static double roundtrip(double x) {
  dsink = (float)x;                // fcvt.s.d
  double r = dsink;
  return r;                        // fcvt.d.s on the way out
}

int main(void) {
  if (d1 + d2 != 3.75)             // fadd.d + feq.d
    return 1;
  if (d2i(d1) != 1)                // fcvt.w.d rtz
    return 2;
  if (i2d(ci) != 7.0)              // fcvt.d.w
    return 3;
  if (!(d1 < d2))                  // flt.d
    return 4;
  if (roundtrip(d1 + d2) != 3.75)  // fcvt.s.d + fcvt.d.s
    return 5;

  if (add9(1, 2, 3, 4, 5, 6, 7, 8, 9) != 45.0) // stack arg + calls
    return 6;

  if (d1 * 1.5 + 2.0 != 4.25)      // f64 constants via 8-byte slots
    return 7;

  if ((d1 > d2 ? d1 : d2) != 2.25) // f64 SELECT via two slots
    return 8;

  return 0;
}
