//===-- control-flow.s - branch, jal and jalr torture, chapter 04 --*- asm -*-===//
//
// main() returns 0 when every control-flow check passes, or the number of the
// first failing check.  Built with llvm-mc -triple=rvmin and linked against
// the course-1 runtime, so the emulator executes real encodings.
//
//===----------------------------------------------------------------------===//

        .text
        .globl  main
        .p2align 2
main:
        addi    sp, sp, -16
        sw      ra, 12(sp)

        // 1. signed compares across the -1/1 boundary
        addi    t0, zero, -1
        addi    t1, zero, 1
        blt     t0, t1, .L1
        jal     zero, .Lfail1
.L1:
        bge     t1, t0, .L2
        jal     zero, .Lfail1
.L2:
        bgeu    t0, t1, .L3             // 0xffffffff >= 1
        jal     zero, .Lfail2
.L3:
        bltu    t1, t0, .L4
        jal     zero, .Lfail2
.L4:

        // 2. INT_MIN < INT_MAX in signed form, and the reverse as unsigned
        lui     t0, 0x80000
        lui     t1, 0x80000
        addi    t1, t1, -1              // 0x7fffffff
        blt     t0, t1, .L5
        jal     zero, .Lfail3
.L5:
        bltu    t1, t0, .L6
        jal     zero, .Lfail3
.L6:

        // 3. jalr clears bit 0 of a computed target
        lui     t0, %hi(.Ltarget)
        addi    t0, t0, %lo(.Ltarget)
        addi    t0, t0, 1               // deliberately odd
        jalr    ra, 0(t0)
        jal     zero, .Lfail4
.Ltarget:

        // 4. backward branch loop: sum 1..10
        addi    t0, zero, 0
        addi    t1, zero, 10
.Lloop:
        add     t0, t0, t1
        addi    t1, t1, -1
        bne     t1, zero, .Lloop
        addi    t2, zero, 55
        beq     t0, t2, .L7
        jal     zero, .Lfail5
.L7:

        // 5. jal links the address of the instruction after it
        jal     ra, .Lcall
.Lafter:
        lui     t0, %hi(.Lafter)
        addi    t0, t0, %lo(.Lafter)
        bne     ra, t0, .Lfail6

        // 6. call and return through ra
        addi    a0, zero, 7
        jal     ra, .Ldouble
        addi    t0, zero, 14
        beq     a0, t0, .L8
        jal     zero, .Lfail8
.L8:

        addi    a0, zero, 0
        jal     zero, .Lreturn

.Lcall:
        jalr    zero, 0(ra)

.Ldouble:
        add     a0, a0, a0
        jalr    zero, 0(ra)

.Lfail1:
        addi    a0, zero, 1
        jal     zero, .Lreturn
.Lfail2:
        addi    a0, zero, 2
        jal     zero, .Lreturn
.Lfail3:
        addi    a0, zero, 3
        jal     zero, .Lreturn
.Lfail4:
        addi    a0, zero, 4
        jal     zero, .Lreturn
.Lfail5:
        addi    a0, zero, 5
        jal     zero, .Lreturn
.Lfail6:
        addi    a0, zero, 6
        jal     zero, .Lreturn
.Lfail8:
        addi    a0, zero, 8
        jal     zero, .Lreturn

.Lreturn:
        lw      ra, 12(sp)
        addi    sp, sp, 16
        jalr    zero, 0(ra)
