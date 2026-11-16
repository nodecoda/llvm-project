/* Chapter 07: the kernel's Sv32 page-table builder. */
#ifndef RMACH_KERNEL_MMU_H
#define RMACH_KERNEL_MMU_H

#include "boot.h"

#include <stdint.h>

/* Build the U-mode address space from boot->seg[], the user stack and UART.
 * Returns 0 on success, -1 when the static table pool is exhausted. */
int mmu_init(const struct boot_info *boot);

/* satp value with MODE=1 and the root table's PPN. */
uint32_t mmu_satp(void);

/* Walk the user page table for a kernel copy (the write syscall).  Returns
 * the physical address, or 0 when va is not a readable U page. */
uint32_t user_translate(uint32_t va);

#endif /* RMACH_KERNEL_MMU_H */
