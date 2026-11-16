/* Chapter 02: prove that the kernel can read and write CSRs. */

static unsigned csrr_mhartid(void) {
  unsigned v;
  __asm__ volatile("csrr %0, mhartid" : "=r"(v));
  return v;
}

static unsigned csrr_misa(void) {
  unsigned v;
  __asm__ volatile("csrr %0, misa" : "=r"(v));
  return v;
}

static unsigned read_mscratch(void) {
  unsigned v;
  __asm__ volatile("csrr %0, mscratch" : "=r"(v));
  return v;
}

static void write_mscratch(unsigned v) {
  __asm__ volatile("csrw mscratch, %0" ::"r"(v));
}

int kmain(void) {
  if (csrr_mhartid() != 0)
    return 1;
  if ((csrr_misa() & 0x1100u) != 0x1100u) /* I (8) and M (12) */
    return 2;
  write_mscratch(0x1234u);
  if (read_mscratch() != 0x1234u)
    return 3;
  return 0;
}
