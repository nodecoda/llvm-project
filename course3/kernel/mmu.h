/* Chapter 07-08: the kernel's Sv32 page-table builder. */
#ifndef RMACH_KERNEL_MMU_H
#define RMACH_KERNEL_MMU_H

#include "boot.h"

#include <stdint.h>

/* Build process pid's U-mode address space from boot->seg[], its stack and
 * the UART.  Returns 0 on success, -1 when the static table pool is
 * exhausted or pid is out of range. */
int mmu_init(uint32_t pid, const struct boot_info *boot);

/* satp value with MODE=1 and process pid's root table PPN. */
uint32_t mmu_satp(uint32_t pid);

/* Walk pid's page table for a kernel copy (the write syscall).  Returns the
 * physical address, or 0 when va is not a readable U page. */
uint32_t user_translate(uint32_t pid, uint32_t va);

#endif /* RMACH_KERNEL_MMU_H */
