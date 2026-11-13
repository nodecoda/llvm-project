# RUN: llvm-mc -triple=rvtrim64 -mattr=+zve64x,+xtinyv -show-encoding %s \
# RUN:   | FileCheck %s --check-prefix=ENC
# RUN: printf '0x8b,0x10,0x20,0xaa\n' \
# RUN:   | llvm-mc -triple=rvtrim64 -mattr=+zve64x,+xtinyv -disassemble \
# RUN:   | FileCheck %s --check-prefix=DIS
# RUN: not llvm-mc -triple=rvtrim64 -mattr=+zve64x -show-encoding %s 2>&1 \
# RUN:   | FileCheck %s --check-prefix=NOEXT

# A vendor instruction needs exactly two inputs: one opcode-database row and one
# auto-td YAML.  The row below is copied verbatim from the database:
#   yushuxin.vfexp 31..26=0x2a vm vs2 19..15=0 14..12=0x1 vd 6..0=0x0b
# so the generated record must encode it bit-for-bit, including the fixed
# vs1=0 field that makes this a unary operation.
#
# Disassembly goes through llvm-mc rather than llvm-objdump: an RVTrim object
# carries the generic RISC-V machine id, so objdump re-enters the stock RISCV
# target, which does not know this vendor extension.

# ENC: yushuxin.vfexp v1, v2{{.*}}# encoding: [0x8b,0x10,0x20,0xaa]
yushuxin.vfexp v1, v2

# The instruction has no masked form: vm is fixed to 1.
# DIS: yushuxin.vfexp v1, v2

# NOEXT: instruction requires the following: 'XTinyV' (YSX tiny vector extension)
