/* Chapter 07: the polling console shared by kmain and the syscall path. */
#ifndef RMACH_KERNEL_UART_H
#define RMACH_KERNEL_UART_H

void uart_init(void);
void uart_putc(char c);
void uart_puts(const char *s);

#endif /* RMACH_KERNEL_UART_H */
