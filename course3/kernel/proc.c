/* Chapter 08: the process table and the round-robin scheduler.
 *
 * Every process has its own root page table and physical arena, but the same
 * user virtual addresses: the switch is exactly "save regs, load regs, write
 * satp/mepc/mscratch".  The trap trampoline stays in trap.S, so scheduling
 * never duplicates the register save/restore code.
 */
#include "proc.h"

#include "mmu.h"

struct proc procs[BOOT_MAX_PROCS];
uint32_t nprocs;
uint32_t current;
int proc_last_exit;

extern void enter_user(uint32_t entry, uint32_t user_sp, uint32_t satp)
    __attribute__((noreturn));

void proc_publish(void) {
  volatile struct boot_result *out =
      (volatile struct boot_result *)BOOT_RESULT_ADDR;
  out->magic = BOOT_RESULT_MAGIC;
  out->nproc = nprocs;
  for (uint32_t i = 0; i < BOOT_MAX_PROCS; ++i) {
    out->proc[i].pid = i;
    out->proc[i].state = i < nprocs ? (uint32_t)procs[i].state : 0xffffffffu;
    out->proc[i].exit_code =
        i < nprocs ? (uint32_t)procs[i].exit_code : 0xffffffffu;
    out->proc[i].pc = i < nprocs ? procs[i].pc : 0;
    out->proc[i].satp = i < nprocs ? procs[i].satp : 0;
  }
}

void proc_init(const struct boot_info *boots, uint32_t count) {
  nprocs = count;
  current = 0;
  proc_last_exit = 0;

  for (uint32_t pid = 0; pid < count; ++pid) {
    if (mmu_init(pid, &boots[pid]) != 0)
      fail(2);

    for (unsigned r = 0; r < 32; ++r)
      procs[pid].regs[r] = 0;
    procs[pid].pc = boots[pid].user_entry;
    procs[pid].satp = mmu_satp(pid);
    procs[pid].state = PROC_RUNNABLE;
    procs[pid].exit_code = 0;
    procs[pid].regs[2] = boots[pid].user_sp; /* sp */
  }

  proc_publish();
}

void proc_start(void) {
  current = 0;
  procs[0].state = PROC_RUNNING;
  proc_publish();
  enter_user(procs[0].pc, procs[0].regs[2], procs[0].satp);
  fail(3); /* enter_user is noreturn */
}

int proc_pick_next(void) {
  if (nprocs == 0)
    return -1;
  for (uint32_t step = 1; step <= nprocs; ++step) {
    const uint32_t pid = (current + step) % nprocs;
    if (procs[pid].state != PROC_EXITED)
      return (int)pid;
  }
  return -1;
}

void proc_switch_to(struct trap_frame *tf, int next) {
  if (next < 0) {
    proc_publish();
    finish((uint32_t)(proc_last_exit & 0xff));
  }

  current = (uint32_t)next;
  procs[current].state = PROC_RUNNING;
  for (unsigned r = 0; r < 32; ++r)
    tf->x[r] = procs[current].regs[r];

  csrw_mepc(procs[current].pc);
  csrw_satp(procs[current].satp);
  /* trap.S swaps sp with mscratch on the way out, so this is the next
   * process's user stack. */
  csrw_mscratch(procs[current].regs[2]);
}
