/* Chapter 07: the boot protocol between rmach and the M-mode kernel. */
#ifndef RMACH_BOOT_H
#define RMACH_BOOT_H

#include <stdint.h>

#define BOOT_MAGIC 0x524d4143u /* "RMAC" */
#define BOOT_MAX_SEGS 8

/* rmach writes this page before entering the kernel. */
#define BOOT_INFO_ADDR 0x800fb000u

/* Physical arena for user PT_LOAD segments and the user stack. */
#define BOOT_USER_BASE 0x80400000u
#define BOOT_USER_SIZE 0x00040000u /* 256 KiB */
#define BOOT_USER_STACK_PHYS 0x80420000u
#define BOOT_USER_STACK_SIZE 0x00004000u /* 16 KiB */
#define BOOT_USER_SP 0x7ff00000u

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

#endif /* RMACH_BOOT_H */
