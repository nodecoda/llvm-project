// Chapter 25: the IR must carry the same data layout as riscv32 and an
// explicit target-abi module flag, otherwise LTO/llc cannot reconstruct ILP32.
//
// RUN: %clang_cc1 -triple rvmin-unknown-elf -target-abi ilp32 -O2 -emit-llvm -o - %s | FileCheck %s
// CHECK: target datalayout = "e-m:e-p:32:32-i64:64-n32-S128"
// CHECK: target triple = "rvmin-unknown-unknown-elf"
// CHECK: !{{[0-9]+}} = !{i32 1, !"target-abi", !"ilp32"}

// -target-feature +m must survive into target-features so a later llc run
// still selects M instructions.
// RUN: %clang_cc1 -triple rvmin-unknown-elf -target-abi ilp32 -target-feature +m -O2 -emit-llvm -o - %s | FileCheck %s --check-prefix=M
// M: "target-features"="+m"

int add(int a, int b) { return a + b; }
int mul(int a, int b) { return a * b; }
