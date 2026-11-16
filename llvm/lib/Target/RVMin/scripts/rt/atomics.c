//===-- atomics.c - A-extension runtime check, chapter 22 --------*- C -*-===//
//
// Compiled with clang (IR contains atomicrmw/cmpxchg), lowered by
// `llc -march=rvmin -mattr=+a`, assembled by llvm-mc, linked against
// rvmin-rt.S + start.S, and executed under QEMU user mode.
//
// What each check is meant to exercise:
//
//   __atomic_fetch_add   amoadd.w.aqrl
//   __atomic_exchange_n  amoswap.w.aqrl
//   __atomic_fetch_and   amoand.w.aqrl   (and or/xor)
//   __atomic_fetch_sub   lr.w.aqrl / sc.w.aqrl loop (no AMO exists for sub)
//   __atomic_compare_exchange_n
//                        lr.w.aqrl / sc.w.aqrl loop, success and failure
//   __atomic_load/store  ordinary lw/sw bracketed by fences
//
// The store-conditional *retry* path needs another hart to steal the
// reservation between lr and sc.  QEMU user mode runs one thread, so this
// program can only prove the value semantics; the lit test checks that the
// retry branch is emitted.
//
// main() returns 0 when every check passes, otherwise the failing check's
// number (which becomes the process exit code).

// These two globals are non-zero on purpose.  They are reset in main()
// anyway, but giving them an initializer puts them in .data instead of .bss.
// With .bss, lld emits a PT_LOAD whose p_filesz is 0 and whose p_vaddr shares
// a page with .text; QEMU 6.2's user-mode loader then zero-fills that whole
// page and the program dies on its first instruction.  Keeping the segment
// file-backed sidesteps that loader quirk and keeps this gate about atomics.
volatile int counter = 0x1234;
volatile int slot = 0x5678;

int main(void) {
  counter = 0;
  for (int i = 0; i < 10; i++) {
    int old = __atomic_fetch_add(&counter, 1, __ATOMIC_SEQ_CST);
    if (old != i)
      return 1;
  }
  if (counter != 10)
    return 2;

  // No AMO for subtraction: this becomes the LL/SC loop.
  int old = __atomic_fetch_sub(&counter, 3, __ATOMIC_SEQ_CST);
  if (old != 10 || counter != 7)
    return 3;

  old = __atomic_exchange_n(&counter, 42, __ATOMIC_SEQ_CST);
  if (old != 7 || counter != 42)
    return 4;

  // cmpxchg success.
  int expected = 42;
  if (!__atomic_compare_exchange_n(&counter, &expected, 50, 0,
                                   __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
    return 5;
  if (counter != 50)
    return 6;

  // cmpxchg failure: expected is updated with the current value.
  expected = 1;
  if (__atomic_compare_exchange_n(&counter, &expected, 99, 0,
                                  __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
    return 7;
  if (expected != 50 || counter != 50)
    return 8;

  __atomic_store_n(&slot, 123, __ATOMIC_SEQ_CST);
  if (__atomic_load_n(&slot, __ATOMIC_SEQ_CST) != 123)
    return 9;

  slot = 0;
  __atomic_fetch_or(&slot, 0x10, __ATOMIC_SEQ_CST);
  __atomic_fetch_and(&slot, 0x1f, __ATOMIC_SEQ_CST);
  __atomic_fetch_xor(&slot, 0x3, __ATOMIC_SEQ_CST);
  if (slot != 0x13)
    return 10;

  return 0;
}
