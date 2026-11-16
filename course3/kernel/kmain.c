/* Chapter 05: poll the UART and say hello, then finish through the test word. */

#define UART_BASE 0x10000000u
#define TEST_BASE 0x00100000u

#define UART_THR (UART_BASE + 0u)
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

static void uart_init(void) {
  outb(UART_LCR, 0x03); /* 8 data bits, no parity, 1 stop bit */
  outb(UART_FCR, 0x07); /* enable and clear the FIFO */
}

static void uart_putc(char c) {
  while ((inb(UART_LSR) & LSR_THRE) == 0)
    ;
  outb(UART_THR, (unsigned char)c);
}

static void uart_puts(const char *s) {
  while (*s)
    uart_putc(*s++);
}

int kmain(void) {
  uart_init();
  uart_puts("hello, rmach\n");

  /* 0x5555 is the pass signature; the upper half carries the exit code. */
  *(volatile unsigned *)TEST_BASE = 0x00005555u;
  for (;;)
    ;
}
