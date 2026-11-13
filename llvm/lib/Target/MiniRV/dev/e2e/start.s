	.text
	.globl _start
_start:
	jal	ra, main
	addi	a7, zero, 93
	ecall
