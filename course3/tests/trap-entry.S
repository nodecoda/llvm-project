# Chapter 06: the M-mode trampoline shared by the MMU tests.  It stays in the
# kernel image and runs untranslated; handle_trap() decides where to mret.
        .section .text,"ax",@progbits
        .globl  trap_entry
trap_entry:
        csrr    a0, mcause
        csrr    a1, mepc
        csrr    a2, mtval
        call    handle_trap
        mret

        .section .note.GNU-stack,"",@progbits
