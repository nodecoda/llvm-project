# RUN: llvm-mc -triple=rvmin -show-encoding %s | FileCheck %s
#
# Chapter 2's six instruction formats, one instruction each.  The bytes are
# checked against the RISC-V reference encoding; the exhaustive cross-check
# with -triple=riscv32 lives in scripts/check-rvmin-encodings.sh.

        add     a0, a1, a2
# CHECK: add a0, a1, a2           # encoding: [0x33,0x85,0xc5,0x00]

        addi    a0, a1, -4
# CHECK: addi a0, a1, -4          # encoding: [0x13,0x85,0xc5,0xff]

        lui     a0, 0x12345
# CHECK: lui a0, 74565            # encoding: [0x37,0x55,0x34,0x12]

        sw      a0, 8(sp)
# CHECK: sw a0, 8(sp)             # encoding: [0x23,0x24,0xa1,0x00]

        beq     a0, a1, 8
# CHECK: beq a0, a1, 8            # encoding: [0x63,0x04,0xb5,0x00]

        jal     ra, 16
# CHECK: jal ra, 16               # encoding: [0xef,0x00,0x00,0x01]

        slli    a0, a1, 7
# CHECK: slli a0, a1, 7           # encoding: [0x13,0x95,0x75,0x00]

        srai    a0, a1, 31
# CHECK: srai a0, a1, 31          # encoding: [0x13,0xd5,0xf5,0x41]
