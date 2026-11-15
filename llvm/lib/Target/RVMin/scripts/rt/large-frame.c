//===-- large-frame.c - a 3 KiB frame under QEMU, chapter 16 --*- C -*-===//
//
// Compiled with clang (RV32I, ilp32) at -O0 and -O2, lowered by
// `llc -march=rvmin`, and executed under QEMU user mode.
//
// Every function below has a frame larger than 2 KiB: the 12-bit immediate of
// `lw`/`sw`/`sb`/`lb` reaches only +/-2048 bytes, so the backend has to
// materialize the address of `buf` into a scratch register.  The volatile
// accesses stop the optimizers from turning the arrays into registers or
// globals.
//
// main() returns 0 when every check passes, otherwise the number of the first
// failing check, which becomes the process exit code.
//
//===----------------------------------------------------------------------===//

#define N 3072

// FNV-1a over a 3 KiB byte array.  The results depend on every byte, so a
// wrong address or a clobbered register shows up as a mismatch.
static unsigned hash_frame(unsigned seed) {
  volatile unsigned char buf[N];
  unsigned i;
  for (i = 0; i < N; i++)
    buf[i] = (unsigned char)(i * 7u + seed);

  // A direct store to the far end of the frame: offset 3071 is well past the
  // 12-bit field, and it is the last byte before the next object.
  buf[N - 1] = (unsigned char)(seed ^ 0x5Au);

  unsigned h = 2166136261u;
  for (i = 0; i < N; i++) {
    h ^= buf[i];
    h *= 16777619u;
  }
  return h;
}

// Recursion with a large frame: depth+1 nested 3 KiB frames, so the frame
// setup, the far accesses and the epilogue all run many times.
static unsigned nest(unsigned depth) {
  volatile unsigned char buf[N];
  buf[0] = (unsigned char)depth;
  buf[N - 1] = (unsigned char)(depth ^ 0x5Au);
  if (depth == 0)
    return (unsigned)buf[0] + buf[N - 1];
  return nest(depth - 1) + (unsigned)buf[0] + buf[N - 1];
}

int main(void) {
  if (hash_frame(1) != 0x90525012u)   // 3 KiB frame, far loads and stores
    return 1;
  if (hash_frame(200) != 0xfea1f97cu) // same, with a different fill pattern
    return 2;
  if (nest(8) != 850u)                // 9 nested 3 KiB frames
    return 3;
  return 0;
}
