# Chapter 06: a position-independent U-mode program.  The kernel copies these
# bytes to 0x80400000 and runs them at two different virtual addresses, so the
# blob may only use PC-relative control flow and absolute device addresses.
#
# It writes "U ok\n" to the UART and then asks for exit through ecall.

        .section .utext,"ax",@progbits
        .globl  u_blob_start
        .globl  u_blob_end
u_blob_start:
        li      t0, 0x10000000       # UART THR
        li      t1, 'U'
        sb      t1, 0(t0)
        li      t1, ' '
        sb      t1, 0(t0)
        li      t1, 'o'
        sb      t1, 0(t0)
        li      t1, 'k'
        sb      t1, 0(t0)
        li      t1, '\n'
        sb      t1, 0(t0)

        li      a7, 93               # SYS_exit
        ecall
u_blob_end:

        .section .note.GNU-stack,"",@progbits
