/* Chapter 08: boot the kernel, build one address space per process and let
 * the CLINT timer preempt them in round-robin order.  With no --user image
 * the kernel falls back to the chapter-05 hello program, so the earlier
 * chapter gates keep working. */

#include "boot.h"
#include "kernel.h"
#include "mmu.h"
#include "proc.h"
#include "uart.h"

#define SYS_WRITE 64
#define SYS_EXIT 93
#define CAUSE_ECALL_U 8
#define CAUSE_TIMER 0x80000007u
#define TIMER_QUANTUM 4000u

extern void trap_entry(void);
extern void enter_user(uint32_t entry, uint32_t user_sp, uint32_t satp);

/* Program the next timer interrupt.  mtime is 64-bit; the CLINT exposes it as
 * two 32-bit words, so read high/low/high to avoid a wrap race. */
static void arm_timer(void) {
  volatile uint32_t *mtime = (volatile uint32_t *)CLINT_MTIME;
  volatile uint32_t *mtimecmp = (volatile uint32_t *)CLINT_MTIMECMP;
  uint32_t hi = mtime[1];
  uint32_t lo = mtime[0];
  if (mtime[1] != hi) {
    hi = mtime[1];
    lo = mtime[0];
  }
  uint64_t next = ((uint64_t)hi << 32) | lo;
  next += TIMER_QUANTUM;
  mtimecmp[0] = (uint32_t)next;
  mtimecmp[1] = (uint32_t)(next >> 32);
}

static void save_current(struct trap_frame *tf) {
  for (unsigned r = 0; r < 32; ++r)
    procs[current].regs[r] = tf->x[r];
}

void handle_trap(struct trap_frame *tf, uint32_t cause, uint32_t epc,
                 uint32_t tval) {
  (void)tval;

  if (cause == CAUSE_TIMER) {
    procs[current].pc = epc; /* resume the interrupted instruction */
    save_current(tf);
    procs[current].state = PROC_RUNNABLE;
    arm_timer();
    proc_switch_to(tf, proc_pick_next());
    return;
  }

  if (cause != CAUSE_ECALL_U)
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
      const uint32_t pa = user_translate(current, buf + i);
      if (pa == 0)
        fail(5);
      uart_putc(*(volatile unsigned char *)pa);
    }
    tf->x[10] = len;
    csrw_mepc(epc + 4); /* the ecall itself already trapped */
    return;
  }

  if (number == SYS_EXIT) {
    procs[current].exit_code = (int)(tf->x[10] & 0xffu);
    procs[current].state = PROC_EXITED;
    proc_last_exit = procs[current].exit_code;
    proc_publish();
    proc_switch_to(tf, proc_pick_next());
    return;
  }

  fail(6); /* unknown syscall */
}

int kmain(struct boot_info *boots, uint32_t count) {
  uart_init();
  if (boots == 0 || boots[0].magic != BOOT_MAGIC)
    fail(1);

  if (count == 0 || boots[0].nseg == 0) { /* no --user: chapter-05 hello */
    uart_puts("hello, rmach\n");
    finish(0);
  }

  if (count > BOOT_MAX_PROCS)
    fail(7);

  proc_init(boots, count);
  csrw_mtvec((uint32_t)trap_entry);
  csrw_mie(MIE_MTIE);
  arm_timer();
  proc_start();
  return 0;
}
