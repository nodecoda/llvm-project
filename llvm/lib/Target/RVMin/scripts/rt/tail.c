//===-- tail.c - one million tail calls under QEMU, chapter 17 ----*- C -*-===//
//
// Compiled with clang at -O2 (which is where the `tail` markers come from),
// lowered by `llc -march=rvmin`, and executed under QEMU user mode.
//
// Every function below has a 16-byte frame (the volatile local forces one).
// Without tail calls, one million levels would need 16 MiB of stack, which is
// more than the 8 MiB the program gets: the test only passes if `return f(x)`
// really became a jump that reuses the frame.
//
// main() returns 0 when every check passes, otherwise the number of the first
// failing check, which becomes the process exit code.
//
//===----------------------------------------------------------------------===//

#define DEPTH 1000000u

// noinline keeps the pair mutually recursive: if clang inlined one into the
// other, TailCallElim would turn the self-recursion into a loop and the test
// would stop exercising the backend.
__attribute__((noinline)) static int is_odd(unsigned n);

__attribute__((noinline)) static int is_even(unsigned n) {
  volatile unsigned marker = n;
  (void)marker;
  if (n == 0)
    return 1;
  return is_odd(n - 1);
}

__attribute__((noinline)) static int is_odd(unsigned n) {
  volatile unsigned marker = n;
  (void)marker;
  if (n == 0)
    return 0;
  return is_even(n - 1);
}

// A second pair that also passes a value through the tail call, so the
// argument registers (not just the frame) are exercised.
__attribute__((noinline)) static unsigned take_even(unsigned n, unsigned acc);

__attribute__((noinline)) static unsigned take_odd(unsigned n, unsigned acc) {
  volatile unsigned marker = n;
  (void)marker;
  if (n == 0)
    return acc;
  return take_even(n - 1, acc + n);
}

__attribute__((noinline)) static unsigned take_even(unsigned n, unsigned acc) {
  volatile unsigned marker = n;
  (void)marker;
  if (n == 0)
    return acc;
  return take_odd(n - 1, acc + n);
}

int main(void) {
  if (is_even(DEPTH) != 1)        // 1e6 nested frames would overflow the stack
    return 1;
  if (is_even(DEPTH - 1) != 0)    // odd parity, one level less
    return 2;
  if (take_even(1000, 0) != 500500u) // 1 + 2 + ... + 1000
    return 3;
  return 0;
}
