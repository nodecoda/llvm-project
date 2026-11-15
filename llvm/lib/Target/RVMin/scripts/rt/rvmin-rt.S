//===-- rvmin-rt.S - minimal runtime for RVMin -------------------*- asm -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Chapter 12: the integer libcalls the code generator emits, written in plain
// RV32I.  No pseudo-instructions, no M extension: only what chapters 1-3 gave
// us.  This is the "libgcc replacement" you need to link a real program.
//
//   __mulsi3   __udivsi3  __umodsi3  __divsi3  __modsi3
//   __ashldi3  __lshrdi3  __ashrdi3
//
// The 64-bit shifts take the value in a0/a1 (low/high) and the count in a2.
// Counts >= 64 are not handled (the compiler never asks for them).
//
//===----------------------------------------------------------------------===//

        .text

//===----------------------------------------------------------------------===//
// unsigned __mulsi3(unsigned a, unsigned b)
// Shift-and-add: look at the low bit of b, add the shifted a when it is set.
//===----------------------------------------------------------------------===//
        .globl  __mulsi3
        .p2align 2
__mulsi3:
        addi    t0, zero, 0             // product
        beq     a1, zero, .Lmul_done
.Lmul_loop:
        andi    t1, a1, 1
        beq     t1, zero, .Lmul_shift
        add     t0, t0, a0
.Lmul_shift:
        slli    a0, a0, 1
        srli    a1, a1, 1
        bne     a1, zero, .Lmul_loop
.Lmul_done:
        addi    a0, t0, 0
        jalr    zero, 0(ra)

//===----------------------------------------------------------------------===//
// unsigned __udivsi3(unsigned n, unsigned d)
// Restoring division, one bit per iteration (32 iterations).
//===----------------------------------------------------------------------===//
        .globl  __udivsi3
        .p2align 2
__udivsi3:
        beq     a1, zero, .Ludiv_zero
        addi    t0, zero, 0             // quotient
        addi    t1, zero, 0             // remainder
        addi    t2, zero, 32            // bit counter
.Ludiv_loop:
        srli    t3, a0, 31              // top bit of the dividend
        slli    a0, a0, 1
        slli    t1, t1, 1
        or      t1, t1, t3              // remainder = (remainder << 1) | bit
        slli    t0, t0, 1
        bltu    t1, a1, .Ludiv_next
        sub     t1, t1, a1
        ori     t0, t0, 1
.Ludiv_next:
        addi    t2, t2, -1
        bne     t2, zero, .Ludiv_loop
        addi    a0, t0, 0
        jalr    zero, 0(ra)
.Ludiv_zero:
        addi    a0, zero, -1            // division by zero: all ones
        jalr    zero, 0(ra)

//===----------------------------------------------------------------------===//
// unsigned __umodsi3(unsigned n, unsigned d)
//===----------------------------------------------------------------------===//
        .globl  __umodsi3
        .p2align 2
__umodsi3:
        beq     a1, zero, .Lumod_zero
        addi    t1, zero, 0
        addi    t2, zero, 32
.Lumod_loop:
        srli    t3, a0, 31
        slli    a0, a0, 1
        slli    t1, t1, 1
        or      t1, t1, t3
        bltu    t1, a1, .Lumod_next
        sub     t1, t1, a1
.Lumod_next:
        addi    t2, t2, -1
        bne     t2, zero, .Lumod_loop
        addi    a0, t1, 0
        jalr    zero, 0(ra)
.Lumod_zero:
        addi    a0, zero, -1
        jalr    zero, 0(ra)

//===----------------------------------------------------------------------===//
// int __divsi3(int a, int b)   -- |a|/|b|, then fix up the sign
//===----------------------------------------------------------------------===//
        .globl  __divsi3
        .p2align 2
__divsi3:
        xor     t4, a0, a1              // sign of the result
        srai    t5, a0, 31
        xor     a0, a0, t5
        sub     a0, a0, t5              // a0 = |a0|
        srai    t5, a1, 31
        xor     a1, a1, t5
        sub     a1, a1, t5              // a1 = |a1|
        addi    sp, sp, -16
        sw      ra, 12(sp)
        sw      t4, 8(sp)
        jal     ra, __udivsi3
        lw      t4, 8(sp)
        lw      ra, 12(sp)
        addi    sp, sp, 16
        bge     t4, zero, .Ldiv_done
        sub     a0, zero, a0
.Ldiv_done:
        jalr    zero, 0(ra)

//===----------------------------------------------------------------------===//
// int __modsi3(int a, int b)   -- the remainder takes the sign of the dividend
//===----------------------------------------------------------------------===//
        .globl  __modsi3
        .p2align 2
__modsi3:
        addi    t4, a0, 0
        srai    t5, a0, 31
        xor     a0, a0, t5
        sub     a0, a0, t5
        srai    t5, a1, 31
        xor     a1, a1, t5
        sub     a1, a1, t5
        addi    sp, sp, -16
        sw      ra, 12(sp)
        sw      t4, 8(sp)
        jal     ra, __umodsi3
        lw      t4, 8(sp)
        lw      ra, 12(sp)
        addi    sp, sp, 16
        bge     t4, zero, .Lmod_done
        sub     a0, zero, a0
.Lmod_done:
        jalr    zero, 0(ra)

//===----------------------------------------------------------------------===//
// long long __ashldi3(long long v, int n)
//===----------------------------------------------------------------------===//
        .globl  __ashldi3
        .p2align 2
__ashldi3:
        beq     a2, zero, .Lshl_done
.Lshl_loop:
        srli    t0, a0, 31              // bit leaving the low word
        slli    a1, a1, 1
        or      a1, a1, t0              // shift into the high word
        slli    a0, a0, 1
        addi    a2, a2, -1
        bne     a2, zero, .Lshl_loop
.Lshl_done:
        jalr    zero, 0(ra)

//===----------------------------------------------------------------------===//
// long long __lshrdi3(long long v, int n)
//===----------------------------------------------------------------------===//
        .globl  __lshrdi3
        .p2align 2
__lshrdi3:
        beq     a2, zero, .Lshr_done
.Lshr_loop:
        andi    t0, a1, 1
        slli    t0, t0, 31
        srli    a0, a0, 1
        or      a0, a0, t0
        srli    a1, a1, 1
        addi    a2, a2, -1
        bne     a2, zero, .Lshr_loop
.Lshr_done:
        jalr    zero, 0(ra)

//===----------------------------------------------------------------------===//
// long long __ashrdi3(long long v, int n)
//===----------------------------------------------------------------------===//
        .globl  __ashrdi3
        .p2align 2
__ashrdi3:
        beq     a2, zero, .Lsar_done
.Lsar_loop:
        andi    t0, a1, 1
        slli    t0, t0, 31
        srli    a0, a0, 1
        or      a0, a0, t0
        srai    a1, a1, 1
        addi    a2, a2, -1
        bne     a2, zero, .Lsar_loop
.Lsar_done:
        jalr    zero, 0(ra)

        .section .note.GNU-stack,"",@progbits
