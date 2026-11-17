; RUN: not llc -march=rvmin -mtriple=riscv32-unknown-elf -O2 -mattr=+v < %s \
; RUN:   -o /dev/null 2>&1 | FileCheck %s --check-prefix=ABI
;
; Chapter 55 only implements the LMUL=1 vector ABI.  An LMUL=2 argument needs
; an even-aligned register pair, so it is rejected instead of being assigned
; to an arbitrary vN.

define void @vec_arg_m2(<8 x i32> %x) {
  ret void
}
; ABI: RVMin: LMUL=2 vector arguments are not supported yet
