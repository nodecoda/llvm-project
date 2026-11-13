; RUN: llc -mtriple=minirv64 -O2 < %s | FileCheck %s

; The base ISA has no A extension, so AtomicExpandPass rewrites every atomic
; IR instruction into a libatomic call.  The second argument of the sized
; __atomic_* entry points is the ordering, encoded with the C11 __ATOMIC_*
; values: relaxed=0, acquire=2, release=3, acq_rel=4, seq_cst=5.

; CHECK-LABEL: atomic_load:
; CHECK:       addi    x11, x0, 5
; CHECK-NEXT:  jal     x1, __atomic_load_8
define i64 @atomic_load(ptr %p) {
  %v = load atomic i64, ptr %p seq_cst, align 8
  ret i64 %v
}

; CHECK-LABEL: atomic_store:
; CHECK:       addi    x12, x0, 3
; CHECK-NEXT:  jal     x1, __atomic_store_8
define void @atomic_store(ptr %p, i64 %v) {
  store atomic i64 %v, ptr %p release, align 8
  ret void
}

; A naturally aligned i32 uses the 4-byte entry point; i8/i16 would use
; __atomic_load_1/__atomic_load_2 the same way.
; CHECK-LABEL: atomic_load_i32:
; CHECK:       addi    x11, x0, 2
; CHECK-NEXT:  jal     x1, __atomic_load_4
define i32 @atomic_load_i32(ptr %p) {
  %v = load atomic i32, ptr %p acquire, align 4
  ret i32 %v
}

; CHECK-LABEL: atomic_xchg:
; CHECK:       jal     x1, __atomic_exchange_8
define i64 @atomic_xchg(ptr %p, i64 %v) {
  %r = atomicrmw xchg ptr %p, i64 %v seq_cst
  ret i64 %r
}

; A compare-exchange passes the success and failure orderings in the last two
; arguments.
; CHECK-LABEL: atomic_cmpxchg:
; CHECK:       addi    x13, x0, 5
; CHECK-NEXT:  addi    x14, x0, 0
; CHECK:       jal     x1, __atomic_compare_exchange_8
define i64 @atomic_cmpxchg(ptr %p, i64 %c, i64 %n) {
  %r = cmpxchg ptr %p, i64 %c, i64 %n seq_cst monotonic
  %v = extractvalue { i64, i1 } %r, 0
  ret i64 %v
}

; CHECK-LABEL: atomic_add:
; CHECK:       jal     x1, __atomic_fetch_add_8
define i64 @atomic_add(ptr %p, i64 %v) {
  %r = atomicrmw add ptr %p, i64 %v monotonic
  ret i64 %r
}

; CHECK-LABEL: atomic_sub:
; CHECK:       addi    x12, x0, 4
; CHECK:       jal     x1, __atomic_fetch_sub_8
define i64 @atomic_sub(ptr %p, i64 %v) {
  %r = atomicrmw sub ptr %p, i64 %v acq_rel
  ret i64 %r
}

; CHECK-LABEL: atomic_and:
; CHECK:       jal     x1, __atomic_fetch_and_8
define i64 @atomic_and(ptr %p, i64 %v) {
  %r = atomicrmw and ptr %p, i64 %v monotonic
  ret i64 %r
}

; CHECK-LABEL: atomic_or:
; CHECK:       jal     x1, __atomic_fetch_or_8
define i64 @atomic_or(ptr %p, i64 %v) {
  %r = atomicrmw or ptr %p, i64 %v monotonic
  ret i64 %r
}

; CHECK-LABEL: atomic_xor:
; CHECK:       jal     x1, __atomic_fetch_xor_8
define i64 @atomic_xor(ptr %p, i64 %v) {
  %r = atomicrmw xor ptr %p, i64 %v monotonic
  ret i64 %r
}

; CHECK-LABEL: atomic_nand:
; CHECK:       jal     x1, __atomic_fetch_nand_8
define i64 @atomic_nand(ptr %p, i64 %v) {
  %r = atomicrmw nand ptr %p, i64 %v monotonic
  ret i64 %r
}

; An operation with no sized entry point, such as max or umin, becomes a
; compare-exchange loop: the first load is followed by the CAS.
; CHECK-LABEL: atomic_max:
; CHECK:       jal     x1, __atomic_load_8
; CHECK:       jal     x1, __atomic_compare_exchange_8
define i64 @atomic_max(ptr %p, i64 %v) {
  %r = atomicrmw max ptr %p, i64 %v monotonic
  ret i64 %r
}

; An under-aligned access cannot use the sized entry point, so it falls back to
; the generic __atomic_load(size, ptr, ret, ordering).
; CHECK-LABEL: atomic_unaligned:
; CHECK:       addi    x10, x0, 8
; CHECK:       jal     x1, __atomic_load
define i64 @atomic_unaligned(ptr %p) {
  %v = load atomic i64, ptr %p monotonic, align 1
  ret i64 %v
}

; AtomicExpandPass leaves a bare fence alone, so the DAG legalizer emits its
; generic full-barrier call.
; CHECK-LABEL: atomic_fence:
; CHECK:       jal     x1, __sync_synchronize
define void @atomic_fence() {
  fence seq_cst
  ret void
}
