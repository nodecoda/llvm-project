# Chapter 07: write(1, msg, 24) where msg starts 6 bytes before a page
# boundary.  A byte-at-a-time user_translate() in the kernel must cross the
# page without losing the second half.
        .section .text.start,"ax",@progbits
        .globl  _start
_start:
        li      a0, 1               # stdout
        la      a1, msg             # page offset 4090
        li      a2, 24
        li      a7, 64              # SYS_write
        ecall

        li      a0, 0
        li      a7, 93              # SYS_exit
        ecall
        j       _start

        .section .rodata
        .balign 4096
        .space  4090, 0x41
msg:
        .ascii  "0123456789abcdefghijklmn"   # 24 bytes, crosses the page

        .section .note.GNU-stack,"",@progbits
