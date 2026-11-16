/* Chapter 07-08: shared kernel primitives. */
#ifndef RMACH_KERNEL_H
#define RMACH_KERNEL_H

#include <stdint.h>

#define TEST_BASE 0x00100000u
#define KERNEL_STACK_TOP 0x80100000u

#define CLINT_BASE 0x02000000u
#define CLINT_MTIMECMP (CLINT_BASE + 0x4000u)
#define CLINT_MTIME (CLINT_BASE + 0xbff8u)

#define MIE_MTIE (1u << 7)
#define MSTATUS_MPIE (1u << 7)
#define MSTATUS_MIE (1u << 3)

/* The frame trap.S builds on the kernel stack: x[i] is saved at 4*i. */
struct trap_frame {
  uint32_t x[32];
};

void finish(uint32_t code) __attribute__((noreturn));
void fail(uint32_t why) __attribute__((noreturn));

static inline void csrw_mtvec(uint32_t value) {
  __asm__ volatile("csrw mtvec, %0" ::"r"(value));
}
static inline void csrw_mepc(uint32_t value) {
  __asm__ volatile("csrw mepc, %0" ::"r"(value));
}
static inline void csrw_mstatus(uint32_t value) {
  __asm__ volatile("csrw mstatus, %0" ::"r"(value));
}
static inline void csrw_mscratch(uint32_t value) {
  __asm__ volatile("csrw mscratch, %0" ::"r"(value));
}
static inline void csrw_satp(uint32_t value) {
  __asm__ volatile("csrw satp, %0" ::"r"(value) : "memory");
}
static inline void csrw_mie(uint32_t value) {
  __asm__ volatile("csrw mie, %0" ::"r"(value));
}

#endif /* RMACH_KERNEL_H */
