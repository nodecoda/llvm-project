/* Chapter 48 end-to-end smoke test for the Zve32x teaching subset.
 *
 * This file is deliberately outside scripts/e2e/ so the chapter 45 matrix
 * (12 programs x 4 -O levels x 6 feature sets = 288 objects) stays unchanged.
 *
 * Build and inspect:
 *   clang --target=rvmin-unknown-elf -march=rv32imafdv -mabi=ilp32d -O2 \
 *         -S -emit-llvm vector.c -o vector.ll
 *   llc -march=rvmin -mtriple=riscv32-unknown-elf -mattr=+v,+d -O2 vector.ll -o -
 *
 * The IR carries the ilp32d module flag from clang, so +d is needed only to
 * satisfy the ABI check; the vector operations themselves come from +v.
 *
 * QEMU 6.2 has no V support, so this chapter verifies the assembly text and
 * that llvm-mc can reassemble it; execution evidence is a chapter 49/50 task.
 */
typedef int v4si __attribute__((vector_size(16)));

void add4(int *out, const int *a, const int *b) {
  v4si va = *(const v4si *)a;
  v4si vb = *(const v4si *)b;
  *(v4si *)out = va + vb;
}
