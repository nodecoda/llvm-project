/* Chapter 07: the polling console moved out of kmain.c. */
#include "uart.h"

#include <stdint.h>

#define UART_BASE 0x10000000u
#define UART_THR (UART_BASE + 0u)
#define UART_FCR (UART_BASE + 2u)
#define UART_LCR (UART_BASE + 3u)
#define UART_LSR (UART_BASE + 5u)

#define LSR_THRE 0x20u

static unsigned inb(unsigned addr) {
  return *(volatile unsigned char *)addr;
}

static void outb(unsigned addr, unsigned value) {
  *(volatile unsigned char *)addr = (unsigned char)value;
}

void uart_init(void) {
  outb(UART_LCR, 0x03); /* 8 data bits, no parity, 1 stop bit */
  outb(UART_FCR, 0x07); /* enable and clear the FIFO */
}

void uart_putc(char c) {
  while ((inb(UART_LSR) & LSR_THRE) == 0)
    ;
  outb(UART_THR, (unsigned char)c);
}

void uart_puts(const char *s) {
  while (*s)
    uart_putc(*s++);
}
