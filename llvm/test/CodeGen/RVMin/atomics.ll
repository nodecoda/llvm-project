; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O2 -mattr=+a < %s | FileCheck %s --check-prefix=A
; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O2 < %s | FileCheck %s --check-prefix=NOA
; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O2 -mattr=+a -stop-after=finalize-isel < %s | FileCheck %s --check-prefix=MIR
; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O0 -mattr=+a -verify-machineinstrs < %s -o /dev/null
; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O2 -mattr=+a < %s -o - \
; RUN:   | llvm-mc -triple=riscv32 -mattr=+a -filetype=obj -o /dev/null
;
; Chapter 22: atomics.
;
; With +a:
;   * atomicrmw operations that have an AMO become one amoxxx.w.aqrl;
;   * sub/nand (no AMO) become a lr.w/sc.w retry loop;
;   * cmpxchg is the same loop;
;   * atomic load/store are an ordinary lw/sw plus the fences that
;     AtomicExpand inserts, because shouldInsertFencesForAtomic is true.
;
; Without +a every atomic is still an __atomic_* libcall: the maximum atomic
; size is zero unless the feature is enabled.
;
; The .aqrl spelling is deliberate: it is correct for every ordering, and the
; chapter documents the extra fence as the price of that simplification.

define i32 @rmw_add(ptr %p, i32 %v) {
  %old = atomicrmw add ptr %p, i32 %v seq_cst
  ret i32 %old
}

define i32 @rmw_and(ptr %p, i32 %v) {
  %old = atomicrmw and ptr %p, i32 %v seq_cst
  ret i32 %old
}

define i32 @rmw_xchg(ptr %p, i32 %v) {
  %old = atomicrmw xchg ptr %p, i32 %v seq_cst
  ret i32 %old
}

define i32 @rmw_umax(ptr %p, i32 %v) {
  %old = atomicrmw umax ptr %p, i32 %v seq_cst
  ret i32 %old
}

define i32 @rmw_sub(ptr %p, i32 %v) {
  %old = atomicrmw sub ptr %p, i32 %v seq_cst
  ret i32 %old
}

define i32 @load_seq_cst(ptr %p) {
  %v = load atomic i32, ptr %p seq_cst, align 4
  ret i32 %v
}

define void @store_seq_cst(ptr %p, i32 %v) {
  store atomic i32 %v, ptr %p seq_cst, align 4
  ret void
}

define i32 @cmpxchg_seq_cst(ptr %p, i32 %expected, i32 %new) {
  %pair = cmpxchg ptr %p, i32 %expected, i32 %new seq_cst seq_cst
  %old = extractvalue { i32, i1 } %pair, 0
  ret i32 %old
}

; A: rmw_add:
; A: amoadd.w.aqrl
; A: rmw_and:
; A: amoand.w.aqrl
; A: rmw_xchg:
; A: amoswap.w.aqrl
; A: rmw_umax:
; A: amomaxu.w.aqrl

; A: rmw_sub:
; A: lr.w.aqrl
; A: sc.w.aqrl

; A: load_seq_cst:
; A: lw
; A: fence

; A: store_seq_cst:
; A: fence
; A: sw

; A: cmpxchg_seq_cst:
; A: lr.w.aqrl
; A: sc.w.aqrl

; The atomic RMW nodes must be gone before the machine pipeline starts: the
; AMO pattern consumed them, so no ATOMIC_LOAD_* remains in the MIR.
; MIR-NOT: ATOMIC_LOAD_ADD
; MIR: AMOADD_W_AQRL
; MIR-NOT: ATOMIC_LOAD_AND
; MIR: AMOAND_W_AQRL

; NOA: rmw_add:
; NOA: __atomic_fetch_add_4
; NOA: rmw_sub:
; NOA: __atomic_fetch_sub_4
; NOA: load_seq_cst:
; NOA-NOT: lr.w
; NOA: __atomic_load_4
; NOA: cmpxchg_seq_cst:
; NOA: __atomic_compare_exchange_4
