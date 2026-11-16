/* Chapter 06: an end-to-end Sv32 load page fault.
 *
 * U-mode runs from a U/R/X page and loads from 0x80000000, which the kernel
 * maps R/W/X but with U=0.  The handler only accepts cause 13 and then writes
 * the pass word, so the gate sees both the translation and the trap path.
 */

#define TEST_BASE 0x00100000u
#define UART_BASE 0x10000000u

#define U_PA 0x80400000u
#define U_VA 0x80001000u
#define KERNEL_VA 0x80000000u

#define PTE_V 0x001u
#define PTE_R 0x002u
#define PTE_W 0x004u
#define PTE_X 0x008u
#define PTE_U 0x010u
#define PTE_A 0x040u
#define PTE_D 0x080u

#define PTE_PA(pa) (((unsigned)(pa) >> 12) << 10)
#define SATP(pa) (0x80000000u | ((unsigned)(pa) >> 12))

extern const unsigned char u_fault_start[], u_fault_end[];
extern void trap_entry(void);

static unsigned root[1024] __attribute__((aligned(4096)));
static unsigned l1[1024] __attribute__((aligned(4096)));
static unsigned l1_uart[1024] __attribute__((aligned(4096)));

static void csrw_mtvec(unsigned value) {
  __asm__ volatile("csrw mtvec, %0" ::"r"(value));
}
static void csrw_mepc(unsigned value) {
  __asm__ volatile("csrw mepc, %0" ::"r"(value));
}
static void csrw_mstatus(unsigned value) {
  __asm__ volatile("csrw mstatus, %0" ::"r"(value));
}
static void csrw_satp(unsigned value) {
  __asm__ volatile("csrw satp, %0" ::"r"(value) : "memory");
}

static void finish(unsigned word) {
  *(volatile unsigned *)TEST_BASE = word;
  for (;;)
    ;
}

void handle_trap(unsigned cause, unsigned epc, unsigned tval) {
  (void)epc;
  (void)tval;
  finish(cause == 13 ? 0x00005555u : 0x00007777u);
}

static void copy_blob(void) {
  volatile unsigned char *dst = (volatile unsigned char *)U_PA;
  const unsigned char *src = u_fault_start;
  while (src < u_fault_end)
    *dst++ = *src++;
}

static void build_tables(void) {
  for (unsigned i = 0; i < 1024; ++i) {
    root[i] = 0;
    l1[i] = 0;
    l1_uart[i] = 0;
  }

  root[U_VA >> 22] = PTE_PA((unsigned)l1) | PTE_V;
  l1[0] = PTE_PA(KERNEL_VA) | PTE_V | PTE_R | PTE_W | PTE_X | PTE_A | PTE_D;
  l1[(U_VA >> 12) & 0x3ff] = PTE_PA(U_PA) | PTE_V | PTE_R | PTE_X | PTE_U | PTE_A;

  root[UART_BASE >> 22] = PTE_PA((unsigned)l1_uart) | PTE_V;
  l1_uart[(UART_BASE >> 12) & 0x3ff] =
      PTE_PA(UART_BASE) | PTE_V | PTE_R | PTE_W | PTE_U | PTE_A | PTE_D;
}

static void enter_u(unsigned entry, unsigned satp) {
  csrw_mstatus(0);
  csrw_mepc(entry);
  csrw_satp(satp);
  __asm__ volatile("mret" ::: "memory");
}

int kmain(void) {
  copy_blob();
  build_tables();
  csrw_mtvec((unsigned)trap_entry);
  enter_u(U_VA, SATP((unsigned)root));
  finish(0x00007777u);
  return 0;
}
