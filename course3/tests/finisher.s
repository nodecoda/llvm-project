# Chapter 05: the SiFive test word carries the exit code in its upper half.

        .section .text.start,"ax",@progbits
        .globl  _start
_start:
        li      t0, 0x00100000
        li      t1, 0x00075555       # exit code 7, "pass" signature
        sw      t1, 0(t0)
        j       _start
        .section .note.GNU-stack,"",@progbits
