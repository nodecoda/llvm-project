// Chapter 26: `clang --target=rvmin` must select the RVMin backend through
// the BareMetal toolchain and pass only the features the backend implements.
//
// RUN: %clang --target=rvmin-unknown-elf -march=rv32i -mabi=ilp32 -### -c %s 2>&1 | FileCheck %s --check-prefix=I
// I: "-cc1"
// I-SAME: "-triple" "rvmin-unknown-unknown-elf"
// I-SAME: "-target-cpu" "generic-rv32"
// I-SAME: "-target-abi" "ilp32"
// I-NOT: "-target-feature" "+m"
// I-NOT: "-target-feature" "+a"
// I-NOT: "-target-feature" "+c"

// RUN: %clang --target=rvmin-unknown-elf -march=rv32im -mabi=ilp32 -### -c %s 2>&1 | FileCheck %s --check-prefix=M
// M: "-target-feature" "+m"

// RUN: %clang --target=rvmin-unknown-elf -mcpu=generic-rv32-m -### -c %s 2>&1 | FileCheck %s --check-prefix=CPU
// CPU: "-target-cpu" "generic-rv32-m"
// CPU: "-target-feature" "+m"

// The integrated assembler consumes the feature list directly, so it must not
// see the RISC-V "-<ext>" tail.
// RUN: %clang --target=rvmin-unknown-elf -march=rv32im -### -c -x assembler /dev/null 2>&1 | FileCheck %s --check-prefix=AS
// AS: "-cc1as"
// AS: "-target-feature" "+m"
// AS-NOT: "-target-feature" "-c"

// Linking goes through ld.lld with the RISC-V ELF emulation.
// RUN: %clang --target=rvmin-unknown-elf -march=rv32i -mabi=ilp32 -nostdlib -fuse-ld=lld -### %s -o %t 2>&1 | FileCheck %s --check-prefix=LINK
// LINK: ld.lld
// LINK-SAME: "-m" "elf32lriscv"
