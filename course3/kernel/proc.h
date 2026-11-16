/* Chapter 08: two U-mode processes, each with its own page table. */
#ifndef RMACH_KERNEL_PROC_H
#define RMACH_KERNEL_PROC_H

#include "boot.h"
#include "kernel.h"

#include <stdint.h>

enum {
  PROC_RUNNABLE = 0,
  PROC_RUNNING = 1,
  PROC_EXITED = 2,
};

struct proc {
  uint32_t regs[32]; /* same layout as struct trap_frame.x */
  uint32_t pc;
  uint32_t satp;
  int state;
  int exit_code;
};

extern struct proc procs[BOOT_MAX_PROCS];
extern uint32_t nprocs;
extern uint32_t current;
extern int proc_last_exit;

/* Build one address space per boot_info record and publish RUNNABLE. */
void proc_init(const struct boot_info *boots, uint32_t count);

/* Enter the first process; never returns to kmain. */
void proc_start(void) __attribute__((noreturn));

/* Round-robin: current+1, current+2, ... skipping EXITED.  Returns -1 when
 * every process has exited. */
int proc_pick_next(void);

/* Copy procs[next] into the trap frame, program mepc/satp/mscratch, and let
 * trap.S restore + mret.  next < 0 publishes results and finishes. */
void proc_switch_to(struct trap_frame *tf, int next);

/* Copy the kernel's process table into BOOT_RESULT_ADDR for --dump-procs. */
void proc_publish(void);

#endif /* RMACH_KERNEL_PROC_H */
