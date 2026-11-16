/* Chapter 07: build the U-mode page tables and copy through them.
 *
 * The kernel runs in M-mode, so its own loads and stores use physical
 * addresses.  The write syscall receives a user virtual buffer; user_translate
 * walks the same table the hardware walks and returns the physical byte.
 */
#include "mmu.h"

#include "kernel.h"

#define PTE_V 0x001u
#define PTE_R 0x002u
#define PTE_W 0x004u
#define PTE_X 0x008u
#define PTE_U 0x010u
#define PTE_A 0x040u
#define PTE_D 0x080u

#define PTE_PPN(pa) (((uint32_t)(pa) >> 12) << 10)
#define MAX_TABLES 8

static uint32_t root[1024] __attribute__((aligned(4096)));
static uint32_t tables[MAX_TABLES][1024] __attribute__((aligned(4096)));
static unsigned used_tables;

static uint32_t *alloc_table(void) {
  if (used_tables >= MAX_TABLES)
    return 0;
  uint32_t *table = tables[used_tables++];
  for (unsigned i = 0; i < 1024; ++i)
    table[i] = 0;
  return table;
}

static int map_page(uint32_t va, uint32_t pa, uint32_t flags) {
  const uint32_t vpn1 = (va >> 22) & 0x3ff;
  const uint32_t vpn0 = (va >> 12) & 0x3ff;
  uint32_t pte = root[vpn1];
  uint32_t *level1;

  if (pte & PTE_V) {
    level1 = (uint32_t *)((pte & 0xfffffc00u) << 2);
  } else {
    level1 = alloc_table();
    if (!level1)
      return -1;
    root[vpn1] = PTE_PPN(level1) | PTE_V;
  }
  level1[vpn0] = PTE_PPN(pa) | flags;
  return 0;
}

static uint32_t seg_flags(uint32_t elf_flags) {
  uint32_t flags = PTE_V | PTE_U | PTE_A;
  if (elf_flags & 4)
    flags |= PTE_R;
  if (elf_flags & 2)
    flags |= PTE_W | PTE_R | PTE_D;
  if (elf_flags & 1)
    flags |= PTE_X;
  if (!(flags & (PTE_R | PTE_W | PTE_X)))
    flags |= PTE_R;
  return flags;
}

int mmu_init(const struct boot_info *boot) {
  for (unsigned i = 0; i < 1024; ++i)
    root[i] = 0;
  used_tables = 0;

  for (uint32_t i = 0; i < boot->nseg; ++i) {
    const struct boot_seg *seg = &boot->seg[i];
    if (seg->memsz == 0)
      continue;

    const uint32_t first = seg->vaddr & ~0xfffu;
    const uint32_t end = seg->vaddr + seg->memsz;
    const uint32_t phys_page = seg->phys & ~0xfffu;
    const uint32_t flags = seg_flags(seg->flags);

    for (uint32_t va = first; va < end; va += 4096) {
      if (map_page(va, phys_page + (va - first), flags) != 0)
        return -1;
    }
  }

  const uint32_t stack_start = BOOT_USER_SP - BOOT_USER_STACK_SIZE;
  for (uint32_t va = stack_start; va < BOOT_USER_SP; va += 4096) {
    const uint32_t pa = BOOT_USER_STACK_PHYS + (va - stack_start);
    if (map_page(va, pa, PTE_V | PTE_R | PTE_W | PTE_U | PTE_A | PTE_D) != 0)
      return -1;
  }

  if (map_page(0x10000000u, 0x10000000u,
               PTE_V | PTE_R | PTE_W | PTE_U | PTE_A | PTE_D) != 0)
    return -1;

  return 0;
}

uint32_t mmu_satp(void) { return 0x80000000u | ((uint32_t)root >> 12); }

uint32_t user_translate(uint32_t va) {
  const uint32_t pte1 = root[(va >> 22) & 0x3ff];
  if (!(pte1 & PTE_V))
    return 0;

  const uint32_t *level1 = (const uint32_t *)((pte1 & 0xfffffc00u) << 2);
  const uint32_t pte0 = level1[(va >> 12) & 0x3ff];
  const uint32_t need = PTE_V | PTE_R | PTE_U | PTE_A;
  if ((pte0 & need) != need)
    return 0;
  return ((pte0 & 0xfffffc00u) << 2) | (va & 0xfffu);
}
