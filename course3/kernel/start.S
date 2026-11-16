//===-- start.S - reset entry for the kernel, chapter 01 ---------*- asm -*-===//
//
// rmach sets sp from the boot protocol; this file sets it again so the image
// is also debuggable by hand, then calls kmain and stops on ebreak.
//
//===----------------------------------------------------------------------===//

        .section .text.start,"ax",@progbits
        .globl  _start
_start:
        li      sp, 0x80100000
        call    kmain
        ebreak
        j       _start

        .section .note.GNU-stack,"",@progbits
