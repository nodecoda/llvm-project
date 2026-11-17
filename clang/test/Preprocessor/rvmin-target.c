// Chapter 25: RVMin reuses the RISC-V macros but only exposes RV32I + M/Zbb/Zbs/A.
//
// RUN: %clang_cc1 -triple rvmin-unknown-elf -E -dM -x c /dev/null | FileCheck %s
// CHECK:      #define __riscv 1
// CHECK-NOT:  __riscv_compressed
// CHECK:      #define __riscv_float_abi_soft 1
// CHECK:      #define __riscv_xlen 32
// CHECK:      #define __rvmin 1

// RUN: %clang_cc1 -triple rvmin-unknown-elf -target-feature +m -E -dM -x c /dev/null | FileCheck %s --check-prefix=M
// M: #define __riscv_div 1
// M: #define __riscv_mul 1

// Chapter 51: C and V are implemented, so the target advertises them instead
// of rejecting the features.
// RUN: %clang_cc1 -triple rvmin-unknown-elf -target-feature +c -E -dM -x c /dev/null | FileCheck %s --check-prefix=C
// C: #define __riscv_compressed 1

// RUN: %clang_cc1 -triple rvmin-unknown-elf -target-feature +v -E -dM -x c /dev/null | FileCheck %s --check-prefix=V
// V: #define __riscv_vector 1

// RUN: not %clang_cc1 -triple rvmin-unknown-elf -target-abi lp64 -E -x c /dev/null 2>&1 | FileCheck %s --check-prefix=ERR-ABI
// ERR-ABI: error: unknown target ABI 'lp64'
