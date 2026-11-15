# RUN: llvm-mc -triple=rvmin -mattr=+m -show-encoding %s | FileCheck %s
# RUN: llvm-mc -triple=rvmin -mcpu=generic-rv32-m -show-encoding %s | FileCheck %s
# RUN: not llvm-mc -triple=rvmin -filetype=obj %s -o /dev/null 2>&1 \
# RUN:   | FileCheck %s --check-prefix=MISSING
#
# Chapter 13: the M extension is behind a feature.  Without it the assembler
# must diagnose the missing feature (it used to accept the instruction and
# emit nothing); with it, all eight encodings are plain OP R-type words.

        mul     a0, a1, a2
# CHECK: mul a0, a1, a2           # encoding: [0x33,0x85,0xc5,0x02]
        mulh    a0, a1, a2
# CHECK: mulh a0, a1, a2          # encoding: [0x33,0x95,0xc5,0x02]
        mulhsu  a0, a1, a2
# CHECK: mulhsu a0, a1, a2        # encoding: [0x33,0xa5,0xc5,0x02]
        mulhu   a0, a1, a2
# CHECK: mulhu a0, a1, a2         # encoding: [0x33,0xb5,0xc5,0x02]
        div     a0, a1, a2
# CHECK: div a0, a1, a2           # encoding: [0x33,0xc5,0xc5,0x02]
        divu    a0, a1, a2
# CHECK: divu a0, a1, a2          # encoding: [0x33,0xd5,0xc5,0x02]
        rem     a0, a1, a2
# CHECK: rem a0, a1, a2           # encoding: [0x33,0xe5,0xc5,0x02]
        remu    a0, a1, a2
# CHECK: remu a0, a1, a2          # encoding: [0x33,0xf5,0xc5,0x02]

# MISSING: error: instruction requires the following: 'M' (Integer Multiplication and Division)
