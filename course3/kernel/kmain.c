/* Chapter 07: boot the kernel, build the U-mode address space and take the
 * first ecall.  With no --user image the kernel falls back to the chapter-05
 * hello program, so the earlier chapter gates keep working. */

#include "boot.h"
#include "kernel.h"
#include "mmu.h"
#include "uart.h"

#define SYS_WRITE 64
#define SYS_EXIT 93

extern void trap_entry(void);
extern void enter_user(uint32_t entry, uint32_t user_sp, uint32_t satp);

void handle_trap(struct trap_frame *tf, uint32_t cause, uint32_t epc,
                 uint32_t tval) {
  (void)tval;
  if (cause != 8) /* only EcallFromU in chapter 07 */
    fail(4);

  const uint32_t number = tf->x[17]; /* a7 */
  if (number == SYS_WRITE) {
    const uint32_t fd = tf->x[10];  /* a0 */
    const uint32_t buf = tf->x[11]; /* a1 */
    const uint32_t len = tf->x[12]; /* a2 */
    if (fd != 1 && fd != 2) {
      tf->x[10] = 0xffffffffu; /* -EBADF, and keep running */
      csrw_mepc(epc + 4);
      return;
    }
    for (uint32_t i = 0; i < len; ++i) {
      const uint32_t pa = user_translate(buf + i);
      if (pa == 0)
        fail(5);
      uart_putc(*(volatile unsigned char *)pa);
    }
    tf->x[10] = len;
    csrw_mepc(epc + 4); /* the ecall itself already trapped */
    return;
  }

  if (number == SYS_EXIT)
    finish(tf->x[10] & 0xffu);

  fail(6); /* unknown syscall */
}

int kmain(struct boot_info *boot) {
  uart_init();
  if (boot == 0 || boot->magic != BOOT_MAGIC)
    fail(1);

  if (boot->nseg == 0) { /* no --user: keep the chapter-05 hello gate alive */
    uart_puts("hello, rmach\n");
    finish(0);
  }

  if (mmu_init(boot) != 0)
    fail(2);

  csrw_mtvec((uint32_t)trap_entry);
  enter_user(boot->user_entry, boot->user_sp, mmu_satp());
  fail(3); /* enter_user is noreturn */
  return 0;
}
