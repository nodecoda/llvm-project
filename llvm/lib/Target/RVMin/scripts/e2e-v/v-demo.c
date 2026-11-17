/* Chapter 49: run the Zve32x teaching subset under QEMU >= 8 user mode.
 *
 * The checks cover the three code-generation paths added so far:
 *   - .vv    : two loaded vectors
 *   - .vi    : a 5-bit constant splat
 *   - .vx    : a constant that does not fit the 5-bit immediate field
 *   - reduce : llvm.vector.reduce.add -> vmv.v.i + vredsum.vs + vmv.x.s
 *
 * QEMU 6.2 and 7.2 do not implement the V extension (7.2 faults on vsetivli);
 * QEMU 8.2 is the first version this course uses for execution.
 */
typedef int v4si __attribute__((vector_size(16)));

volatile int a[4] = {1, 2, 3, 4};
volatile int b[4] = {10, 20, 30, 40};
volatile int out[4];

int main(void) {
  v4si va = *(const v4si *)a;
  v4si vb = *(const v4si *)b;

  *(v4si *)out = va + vb;
  if (out[0] != 11 || out[1] != 22 || out[2] != 33 || out[3] != 44)
    return 1;

  v4si small = va + (v4si){5, 5, 5, 5};
  if (small[0] != 6 || small[3] != 9)
    return 2;

  v4si large = va + (v4si){100, 100, 100, 100};
  if (large[0] != 101 || large[3] != 104)
    return 3;

  if (__builtin_reduce_add(va) != 10)
    return 4;

  return 0;
}
