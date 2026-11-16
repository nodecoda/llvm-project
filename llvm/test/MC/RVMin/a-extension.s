# RUN: split-file %s %t
# RUN: llvm-mc -triple=rvmin -mattr=+a -show-encoding %t/all.s | FileCheck %s
# RUN: not llvm-mc -triple=rvmin -filetype=obj %t/all.s -o /dev/null 2>&1 | FileCheck %s --check-prefix=NOA
# RUN: llvm-mc -triple=rvmin -mattr=+a -disassemble %t/hex.txt | FileCheck %s --check-prefix=DIS
#
# Chapter 22: the A extension.  Every encoding below is byte-identical to the
# reference RISC-V assembler (`llvm-mc -triple=riscv32 -mattr=+a`), and
# scripts/check-rvmin-encodings.sh keeps re-checking that.
#
# `(a1)` is a zero-displacement memory operand.  The printer emits exactly
# that form, so accepting it in the parser is what makes the
# asm -> obj -> disasm -> asm round trip close.

#--- all.s
lr.w a0, (a1)
lr.w.aq a0, (a1)
lr.w.rl a0, (a1)
lr.w.aqrl a0, (a1)
sc.w a0, a2, (a1)
sc.w.aqrl a0, a2, (a1)
amoswap.w a0, a2, (a1)
amoadd.w a0, a2, (a1)
amoxor.w a0, a2, (a1)
amoand.w a0, a2, (a1)
amoor.w a0, a2, (a1)
amomin.w a0, a2, (a1)
amomax.w a0, a2, (a1)
amominu.w a0, a2, (a1)
amomaxu.w a0, a2, (a1)
amoadd.w.aqrl a0, a2, (a1)
fence
fence.i

# CHECK: lr.w a0, (a1)                        # encoding: [0x2f,0xa5,0x05,0x10]
# CHECK-NEXT: lr.w.aq a0, (a1)                # encoding: [0x2f,0xa5,0x05,0x14]
# CHECK-NEXT: lr.w.rl a0, (a1)                # encoding: [0x2f,0xa5,0x05,0x12]
# CHECK-NEXT: lr.w.aqrl a0, (a1)              # encoding: [0x2f,0xa5,0x05,0x16]
# CHECK-NEXT: sc.w a0, a2, (a1)               # encoding: [0x2f,0xa5,0xc5,0x18]
# CHECK-NEXT: sc.w.aqrl a0, a2, (a1)          # encoding: [0x2f,0xa5,0xc5,0x1e]
# CHECK-NEXT: amoswap.w a0, a2, (a1)          # encoding: [0x2f,0xa5,0xc5,0x08]
# CHECK-NEXT: amoadd.w a0, a2, (a1)           # encoding: [0x2f,0xa5,0xc5,0x00]
# CHECK-NEXT: amoxor.w a0, a2, (a1)           # encoding: [0x2f,0xa5,0xc5,0x20]
# CHECK-NEXT: amoand.w a0, a2, (a1)           # encoding: [0x2f,0xa5,0xc5,0x60]
# CHECK-NEXT: amoor.w a0, a2, (a1)            # encoding: [0x2f,0xa5,0xc5,0x40]
# CHECK-NEXT: amomin.w a0, a2, (a1)           # encoding: [0x2f,0xa5,0xc5,0x80]
# CHECK-NEXT: amomax.w a0, a2, (a1)           # encoding: [0x2f,0xa5,0xc5,0xa0]
# CHECK-NEXT: amominu.w a0, a2, (a1)          # encoding: [0x2f,0xa5,0xc5,0xc0]
# CHECK-NEXT: amomaxu.w a0, a2, (a1)          # encoding: [0x2f,0xa5,0xc5,0xe0]
# CHECK-NEXT: amoadd.w.aqrl a0, a2, (a1)      # encoding: [0x2f,0xa5,0xc5,0x06]
# CHECK-NEXT: fence                           # encoding: [0x0f,0x00,0xf0,0x0f]
# CHECK-NEXT: fence.i                         # encoding: [0x0f,0x10,0x00,0x00]

# Without +a the assembler must name the missing feature instead of silently
# accepting the mnemonic.
# NOA: error: instruction requires the following: 'A' (Atomic Instructions)

#--- hex.txt
# lr.w a0, (a1)
0x2f 0xa5 0x05 0x10
# sc.w.aqrl a0, a2, (a1)
0x2f 0xa5 0xc5 0x1e
# amoadd.w a0, a2, (a1)
0x2f 0xa5 0xc5 0x00
# fence
0x0f 0x00 0xf0 0x0f
# fence.i
0x0f 0x10 0x00 0x00

# DIS: {{^}}	lr.w a0, (a1){{$}}
# DIS-NEXT: {{^}}	sc.w.aqrl a0, a2, (a1){{$}}
# DIS-NEXT: {{^}}	amoadd.w a0, a2, (a1){{$}}
# DIS-NEXT: {{^}}	fence{{$}}
# DIS-NEXT: {{^}}	fence.i{{$}}
