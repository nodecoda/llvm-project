; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O2 -verify-machineinstrs < %s | FileCheck %s --check-prefix=RV32I
; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O2 -mattr=+m -verify-machineinstrs < %s | FileCheck %s --check-prefix=M
; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O2 -mattr=+m < %s | FileCheck %s --check-prefix=MS
; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O0 < %s | FileCheck %s --check-prefix=O0
; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O2 < %s \
; RUN:   | llvm-mc -triple=rvmin -filetype=obj -o /dev/null
; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O2 -mattr=+m < %s \
; RUN:   | llvm-mc -triple=rvmin -mattr=+m -filetype=obj -o /dev/null

; Chapter 19: constant multiplies and divisions.
;
;   x*3   ->  (x<<2) - x          two instructions, with or without M
;   x*5   ->  (x<<2) + x
;   x*7   ->  (x<<3) - x
;   x*10  ->  (x<<3) + (x<<1)     without M, avoids __mulsi3
;   x*64  ->  (x<<6)              generic combiner
;
; Division by a constant is the generic magic-number expansion, which only
; needs MULHU/MULHS to be legal -- no target code.  Under `minsize` the cost
; model prefers one div/rem instruction instead (isIntDivCheap).

define i32 @mul3(i32 %x) { %r = mul i32 %x, 3 ret i32 %r }
define i32 @mul5(i32 %x) { %r = mul i32 %x, 5 ret i32 %r }
define i32 @mul7(i32 %x) { %r = mul i32 %x, 7 ret i32 %r }
define i32 @mul10(i32 %x) { %r = mul i32 %x, 10 ret i32 %r }
define i32 @mul64(i32 %x) { %r = mul i32 %x, 64 ret i32 %r }
define i32 @div7(i32 %x) { %r = sdiv i32 %x, 7 ret i32 %r }
define i32 @mod10(i32 %x) { %r = srem i32 %x, 10 ret i32 %r }
define i32 @div7_ms(i32 %x) minsize { %r = sdiv i32 %x, 7 ret i32 %r }
define i32 @mod10_ms(i32 %x) minsize { %r = srem i32 %x, 10 ret i32 %r }

; Without M there is no multiply instruction at all: the shifts and adds are
; the only reason these functions do not call __mulsi3.
; RV32I-LABEL: mul3:
; RV32I:    slli a1, a0, 2
; RV32I-NEXT:    sub a0, a1, a0
; RV32I-LABEL: mul5:
; RV32I:    slli a1, a0, 2
; RV32I-NEXT:    add a0, a1, a0
; RV32I-LABEL: mul7:
; RV32I:    slli a1, a0, 3
; RV32I-NEXT:    sub a0, a1, a0
; RV32I-LABEL: mul10:
; RV32I:    slli a1, a0, 3
; RV32I-NEXT:    slli a0, a0, 1
; RV32I-NEXT:    add a0, a0, a1
; RV32I-LABEL: mul64:
; RV32I:    slli a0, a0, 6
; RV32I-NOT:     __mulsi3
; Division still needs the runtime without M.
; RV32I-LABEL: div7:
; RV32I:         %hi(__divsi3)

; With M the shift/add forms of 3/5/7 stay (same size, no multiply), while a
; two-set-bit constant is better off as a real mul.
; M-LABEL: mul3:
; M:        slli a1, a0, 2
; M-NEXT:        sub a0, a1, a0
; M-LABEL: mul10:
; M:        addi a1, zero, 10
; M-NEXT:        mul a0, a0, a1
; M-LABEL: div7:
; M:        lui a1, 599186
; M-NEXT:        addi a1, a1, 1171
; M-NEXT:        mulh a1, a0, a1
; M-NOT:         __divsi3
; M-LABEL: mod10:
; M-NOT:         __modsi3
; M:             mulh
; M-LABEL: div7_ms:
; M:        addi a1, zero, 7
; M-NEXT:        div a0, a0, a1
; M-LABEL: mod10_ms:
; M:        addi a1, zero, 10
; M-NEXT:        rem a0, a0, a1

; The target combine is not an optimization-level feature: -O0 gets the same
; shift/add forms (the DAG combiner runs there too).
; O0-LABEL: mul3:
; O0:       slli a0, a1, 2
; O0-NEXT:       sub a0, a0, a1

; The MS run checks that minsize functions use div/rem; the rest is unchanged.
; MS-LABEL: div7_ms:
; MS:       addi a1, zero, 7
; MS-NEXT:       div a0, a0, a1
; MS-LABEL: mod10_ms:
; MS:       addi a1, zero, 10
; MS-NEXT:       rem a0, a0, a1
