//===-- abi-soft.c - chapter 51 soft-float ABI smoke test ------*- C -*-===//
//
// The shared float.c/double.c corpora are optimized under -O2, which can fold
// the interesting call boundaries away (a constant add9f call disappears, and
// i2f returns a volatile global loaded from memory).  This program feeds every
// helper from a volatile object and marks it noinline, so clang must keep the
// call and the value really crosses the soft-float ABI boundary:
//
//   fpass(float)   : a0  -> fa0 (fmv.w.x) -> a0 (fmv.x.w)
//   dpass(double)  : a0/a1 -> fa0 -> a0/a1 (via the 8-byte stack slot)
//   f2i(float)     : a0  -> fa0 (fmv.w.x) -> fcvt.w.s
//   i2f(int)       : a0  -> fcvt.s.w -> a0 (fmv.x.w)
//
// Returns 0 when every check passes, otherwise the number of the first
// failing check, matching the rest of the freestanding runtime corpus.
//
//===----------------------------------------------------------------------===//

volatile float fsrc = 1.5f;
volatile float fdst;
volatile double dsrc = 2.25;
volatile double ddst;
volatile int isrc = 7;
volatile int isink;

// The arithmetic keeps the call alive: LLVM's `returned` attribute would
// otherwise let clang replace pass-through calls with their arguments and
// the ABI conversion would never be emitted.
__attribute__((noinline)) static float fpass(float x) { return x * 2.0f; }

__attribute__((noinline)) static double dpass(double x) { return x * 2.0; }

__attribute__((noinline)) static int f2i(float x) {
  isink = (int)x;
  return isink;
}

__attribute__((noinline)) static float i2f(int x) {
  fdst = (float)x;
  return fdst;
}

int main(void) {
  float f = fpass(fsrc);          // f32 argument/return cross in a0 bit patterns
  fdst = f;
  if (f != 3.0f)
    return 1;

  double d = dpass(dsrc);         // f64 argument and return cross in a0/a1
  ddst = d;
  if (d != 4.5)
    return 2;

  if (f2i(fsrc) != 1)             // GPR -> FPR on the argument side
    return 3;

  if (i2f(isrc) != 7.0f)          // FPR -> GPR on the return side
    return 4;

  return 0;
}
