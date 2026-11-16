/* Chapter 07: the SiFive test word is the kernel's only exit path. */
#include "kernel.h"

#include "uart.h"

void finish(uint32_t code) {
  /* 0x5555 is the pass signature; the upper half carries the exit code. */
  *(volatile uint32_t *)TEST_BASE = 0x00005555u | ((code & 0xffffu) << 16);
  for (;;)
    ;
}

void fail(uint32_t why) {
  /* Failure output is diagnostic only; successful runs stay silent. */
  uart_puts("rmach kernel: fail ");
  if (why >= 10)
    uart_putc((char)('0' + (why / 10) % 10));
  uart_putc((char)('0' + why % 10));
  uart_putc('\n');
  *(volatile uint32_t *)TEST_BASE = 0x00007777u;
  for (;;)
    ;
}
