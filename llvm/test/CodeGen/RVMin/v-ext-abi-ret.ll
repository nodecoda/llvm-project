; RUN: not llc -march=rvmin -mtriple=riscv32-unknown-elf -O2 -mattr=+v < %s \
; RUN:   -o /dev/null 2>&1 | FileCheck %s --check-prefix=ABI
;
; Vector returns need the same unimplemented calling convention.

define <4 x i32> @vec_ret() {
  ret <4 x i32> zeroinitializer
}
; ABI: RVMin: vector returns are not supported yet
