/* Chapter 07-08: the boot protocol between rmach and the M-mode kernel. */
#ifndef RMACH_BOOT_H
#define RMACH_BOOT_H

#include <stdint.h>

#define BOOT_MAGIC 0x524d4143u /* "RMAC" */
#define BOOT_MAX_SEGS 8
#define BOOT_MAX_PROCS 2

/* rmach writes an array of boot_info records at this page. */
#define BOOT_INFO_ADDR 0x800fb000u

/* The kernel publishes per-process results here; rmach --dump-procs reads it. */
#define BOOT_RESULT_ADDR 0x800fa000u
#define BOOT_RESULT_MAGIC 0x52534c54u /* "RSLT" */

/* Each process owns BOOT_USER_STRIDE bytes: an image arena, then its stack. */
#define BOOT_USER_BASE 0x80400000u
#define BOOT_USER_STRIDE 0x00040000u /* 256 KiB per process */
#define BOOT_USER_STACK_OFFSET 0x00020000u
#define BOOT_USER_STACK_SIZE 0x00004000u /* 16 KiB */
#define BOOT_USER_SIZE (BOOT_USER_STRIDE * BOOT_MAX_PROCS)
#define BOOT_USER_SP 0x7ff00000u

#define BOOT_USER_IMAGE_BASE(pid) \
  (BOOT_USER_BASE + (uint32_t)(pid) * BOOT_USER_STRIDE)
#define BOOT_USER_STACK_PHYS(pid) \
  (BOOT_USER_IMAGE_BASE(pid) + BOOT_USER_STACK_OFFSET)

/* One entry is a page-level placement, not an ELF segment: ELF segments may
 * share a 4 KiB page, so the loader merges them and records the union of the
 * permissions.  vaddr/phys are page-aligned and memsz is normally 4096. */
struct boot_seg {
  uint32_t vaddr;
  uint32_t phys;
  uint32_t filesz; /* bytes present in the host image (normally 4096) */
  uint32_t memsz;
  uint32_t flags;  /* union of PF_X=1, PF_W=2, PF_R=4 */
};

struct boot_info {
  uint32_t magic;
  uint32_t user_entry;
  uint32_t user_sp;
  uint32_t nseg;
  struct boot_seg seg[BOOT_MAX_SEGS];
};

/* Written by the kernel, read by --dump-procs.  One record per process. */
struct boot_proc_result {
  uint32_t pid;
  uint32_t state; /* 0 runnable, 1 running, 2 exited */
  uint32_t exit_code;
  uint32_t pc;
};

struct boot_result {
  uint32_t magic;
  uint32_t nproc;
  struct boot_proc_result proc[BOOT_MAX_PROCS];
};

#endif /* RMACH_BOOT_H */
