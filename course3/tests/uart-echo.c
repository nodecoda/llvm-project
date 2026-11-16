/* Chapter 05: echo bytes from --input until a newline, then finish. */

#define UART_BASE 0x10000000u
#define TEST_BASE 0x00100000u

#define UART_THR (UART_BASE + 0u)
#define UART_RBR (UART_BASE + 0u)
#define UART_FCR (UART_BASE + 2u)
#define UART_LCR (UART_BASE + 3u)
#define UART_LSR (UART_BASE + 5u)

#define LSR_DR 0x01u
#define LSR_THRE 0x20u

static unsigned inb(unsigned addr) {
  return *(volatile unsigned char *)addr;
}

static void outb(unsigned addr, unsigned value) {
  *(volatile unsigned char *)addr = (unsigned char)value;
}

static void putc(char c) {
  while ((inb(UART_LSR) & LSR_THRE) == 0)
    ;
  outb(UART_THR, (unsigned char)c);
}

int kmain(void) {
  outb(UART_LCR, 0x03);
  outb(UART_FCR, 0x07);

  for (;;) {
    while ((inb(UART_LSR) & LSR_DR) == 0)
      ;
    char c = (char)inb(UART_RBR);
    putc(c);
    if (c == '\n')
      break;
  }

  *(volatile unsigned *)TEST_BASE = 0x00005555u;
  for (;;)
    ;
}
