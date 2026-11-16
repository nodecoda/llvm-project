/* Chapter 06: run the same U-mode blob under two Sv32 mappings.
 *
 * Phase 1 maps VA 0x80001000 -> PA 0x80400000 (identity-ish: the VA is where
 * the kernel image lives, but the PA is the user page).  Phase 2 maps
 * VA 0x00010000 -> the same PA.  Both phases must print "U ok\n"; the M-mode
 * ecall handler switches satp between them and then writes the test word.
 */

#define TEST_BASE 0x00100000u
#define UART_BASE 0x10000000u

#define U_PA 0x80400000u
#define U_ID_VA 0x80001000u
#define U_RELOC_VA 0x00010000u

#define PTE_V 0x001u
#define PTE_R 0x002u
#define PTE_W 0x004u
#define PTE_X 0x008u
#define PTE_U 0x010u
#define PTE_A 0x040u
#define PTE_D 0x080u

#define PTE_PA(pa) (((unsigned)(pa) >> 12) << 10)
#define SATP(pa) (0x80000000u | ((unsigned)(pa) >> 12))

extern const unsigned char u_blob_start[], u_blob_end[];
extern void trap_entry(void);

static unsigned root_id[1024] __attribute__((aligned(4096)));
static unsigned root_reloc[1024] __attribute__((aligned(4096)));
static unsigned l1_id[1024] __attribute__((aligned(4096)));
static unsigned l1_reloc[1024] __attribute__((aligned(4096)));
static unsigned l1_uart_id[1024] __attribute__((aligned(4096)));
static unsigned l1_uart_reloc[1024] __attribute__((aligned(4096)));

static volatile unsigned phase;

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
  if (cause != 8) /* not EcallFromU */
    finish(0x00007777u);

  if (phase == 0) {
    phase = 1;
    csrw_satp(SATP((unsigned)root_reloc));
    csrw_mepc(U_RELOC_VA);
    csrw_mstatus(0); /* MPP=U, MIE=0 */
    return;
  }
  finish(0x00005555u);
}

static void copy_blob(void) {
  volatile unsigned char *dst = (volatile unsigned char *)U_PA;
  const unsigned char *src = u_blob_start;
  while (src < u_blob_end)
    *dst++ = *src++;
}

static void build_tables(void) {
  for (unsigned i = 0; i < 1024; ++i) {
    root_id[i] = 0;
    root_reloc[i] = 0;
    l1_id[i] = 0;
    l1_reloc[i] = 0;
    l1_uart_id[i] = 0;
    l1_uart_reloc[i] = 0;
  }

  const unsigned uart =
      PTE_PA(UART_BASE) | PTE_V | PTE_R | PTE_W | PTE_U | PTE_A | PTE_D;
  const unsigned ucode = PTE_PA(U_PA) | PTE_V | PTE_R | PTE_X | PTE_U | PTE_A;

  /* Phase 1: VA 0x80001000 (VPN1=0x200, VPN0=1) -> PA 0x80400000. */
  root_id[U_ID_VA >> 22] = PTE_PA((unsigned)l1_id) | PTE_V;
  l1_id[(U_ID_VA >> 12) & 0x3ff] = ucode;
  root_id[UART_BASE >> 22] = PTE_PA((unsigned)l1_uart_id) | PTE_V;
  l1_uart_id[(UART_BASE >> 12) & 0x3ff] = uart;

  /* Phase 2: VA 0x00010000 (VPN1=0, VPN0=0x10) -> the same PA. */
  root_reloc[U_RELOC_VA >> 22] = PTE_PA((unsigned)l1_reloc) | PTE_V;
  l1_reloc[(U_RELOC_VA >> 12) & 0x3ff] = ucode;
  root_reloc[UART_BASE >> 22] = PTE_PA((unsigned)l1_uart_reloc) | PTE_V;
  l1_uart_reloc[(UART_BASE >> 12) & 0x3ff] = uart;
}

static void enter_u(unsigned entry, unsigned satp) {
  csrw_mstatus(0); /* MPP=U */
  csrw_mepc(entry);
  csrw_satp(satp);
  __asm__ volatile("mret" ::: "memory");
}

int kmain(void) {
  copy_blob();
  build_tables();
  csrw_mtvec((unsigned)trap_entry);
  phase = 0;
  enter_u(U_ID_VA, SATP((unsigned)root_id));
  finish(0x00007777u);
  return 0;
}
