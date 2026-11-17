// Chapter 26: driver-level rejection for combinations the RVMin backend cannot
// lower.
//
// RUN: not %clang --target=rvmin-unknown-elf -march=rv64i -c %s -o %t.o 2>&1 | FileCheck %s --check-prefix=RV64
// RV64: unsupported argument 'rv64i' to option '-march' for target 'rvmin-unknown-unknown-elf'

// RUN: not %clang --target=rvmin-unknown-elf -march=native -c %s -o %t.o 2>&1 | FileCheck %s --check-prefix=NATIVE
// NATIVE: unsupported argument 'native' to option '-march' for target 'rvmin-unknown-unknown-elf'

// RUN: not %clang --target=rvmin-unknown-elf -fPIC -c %s -o %t.o 2>&1 | FileCheck %s --check-prefix=PIC
// PIC: unsupported option '-fPIC/-fPIE' for target 'rvmin-unknown-unknown-elf'

// RUN: not %clang --target=rvmin-unknown-elf -mcmodel=large -c %s -o %t.o 2>&1 | FileCheck %s --check-prefix=CM
// CM: unsupported argument 'large' to option '-mcmodel=' for target 'rvmin-unknown-unknown-elf'

// RUN: not %clang --target=rvmin-unknown-elf -mrelax -c %s -o %t.o 2>&1 | FileCheck %s --check-prefix=RELAX
// RELAX: unsupported argument 'relax' to option '-mrelax' for target 'rvmin-unknown-unknown-elf'

// Chapter 28 AC7: every ISA extension the backend cannot lower must be
// diagnosed by the driver, not silently stripped from the feature string.
// Chapter 51: C, F and V are implemented now, so only the extensions that are
// still genuinely unsupported remain in this block.
// RUN: not %clang --target=rvmin-unknown-elf -march=rv32i_zba -c %s -o %t.o 2>&1 | FileCheck %s --check-prefix=EXT
// RUN: not %clang --target=rvmin-unknown-elf -march=rv32e -c %s -o %t.o 2>&1 | FileCheck %s --check-prefix=EXT
// EXT: unsupported argument '{{rv32.*}}' to option '-march' for target 'rvmin-unknown-unknown-elf'

// RUN: not %clang --target=rvmin-unknown-elf -mabi=lp64 -c %s -o %t.o 2>&1 | FileCheck %s --check-prefix=ABI
// ABI: unknown target ABI 'lp64'
