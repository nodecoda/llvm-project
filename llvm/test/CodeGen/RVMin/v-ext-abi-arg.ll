; RUN: not llc -march=rvmin -mtriple=riscv32-unknown-elf -O2 -mattr=+v < %s \
; RUN:   -o /dev/null 2>&1 | FileCheck %s --check-prefix=ABI
;
; Chapter 48 does not implement the vector calling convention.  A vector
; argument must be rejected instead of silently passed as integers.

define void @vec_arg(<4 x i32> %x) {
  ret void
}
; ABI: RVMin: vector arguments are not supported yet
