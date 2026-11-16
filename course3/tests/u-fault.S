# Chapter 06: a U-mode blob that must fail.  It loads from 0x80000000, a kernel
# page with U=0; the machine must report LoadPageFault (cause 13).
        .section .utext,"ax",@progbits
        .globl  u_fault_start
        .globl  u_fault_end
u_fault_start:
        li      t0, 0x80000000
        lw      t1, 0(t0)            # U=0 page -> cause 13
        li      a7, 93
        ecall                        # must not be reached
u_fault_end:

        .section .note.GNU-stack,"",@progbits
