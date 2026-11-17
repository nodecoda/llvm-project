# RUN: llvm-mc -triple=rvmin -mattr=+f -show-encoding %s | FileCheck %s
# RUN: llvm-mc -triple=rvmin -mcpu=generic-rv32-f -show-encoding %s | FileCheck %s
# RUN: not llvm-mc -triple=rvmin -filetype=obj %s -o /dev/null 2>&1 \
# RUN:   | FileCheck %s --check-prefix=MISSING
# RUN: llvm-mc -triple=rvmin -mattr=+f -filetype=obj %s -o %t.o
# RUN: llvm-objdump -d --mattr=+f %t.o | FileCheck %s --check-prefix=DIS
#
# Chapter 40: the F extension.  Every encoding below is byte-identical to the
# reference RISC-V assembler (`llvm-mc -triple=riscv32 -mattr=+f`), and
# scripts/check-rvmin-encodings.sh re-checks that on every run.
#
# The `rtz` spelling is not a register or a symbol: it is a literal token in
# the instruction's AsmString, which is why the parser produces a token
# operand for it.  That is what makes the asm -> obj -> disasm -> asm round
# trip close for `fcvt.w.s a0, fa1, rtz`.

        flw     fa0, 0(a0)
# CHECK: flw fa0, 0(a0)           # encoding: [0x07,0x25,0x05,0x00]
        fsw     fa1, 4(a1)
# CHECK: fsw fa1, 4(a1)           # encoding: [0x27,0xa2,0xb5,0x00]

        fmadd.s fa0, fa1, fa2, fa3
# CHECK: fmadd.s fa0, fa1, fa2, fa3 # encoding: [0x43,0xf5,0xc5,0x68]
        fmsub.s fa0, fa1, fa2, fa3
# CHECK: fmsub.s fa0, fa1, fa2, fa3 # encoding: [0x47,0xf5,0xc5,0x68]
        fnmsub.s fa0, fa1, fa2, fa3
# CHECK: fnmsub.s fa0, fa1, fa2, fa3 # encoding: [0x4b,0xf5,0xc5,0x68]
        fnmadd.s fa0, fa1, fa2, fa3
# CHECK: fnmadd.s fa0, fa1, fa2, fa3 # encoding: [0x4f,0xf5,0xc5,0x68]

        fadd.s  fa0, fa1, fa2
# CHECK: fadd.s fa0, fa1, fa2     # encoding: [0x53,0xf5,0xc5,0x00]
        fsub.s  fa0, fa1, fa2
# CHECK: fsub.s fa0, fa1, fa2     # encoding: [0x53,0xf5,0xc5,0x08]
        fmul.s  fa0, fa1, fa2
# CHECK: fmul.s fa0, fa1, fa2     # encoding: [0x53,0xf5,0xc5,0x10]
        fdiv.s  fa0, fa1, fa2
# CHECK: fdiv.s fa0, fa1, fa2     # encoding: [0x53,0xf5,0xc5,0x18]
        fsqrt.s fa0, fa1
# CHECK: fsqrt.s fa0, fa1         # encoding: [0x53,0xf5,0x05,0x58]

        fsgnj.s fa0, fa1, fa2
# CHECK: fsgnj.s fa0, fa1, fa2    # encoding: [0x53,0x85,0xc5,0x20]
        fsgnjn.s fa0, fa1, fa2
# CHECK: fsgnjn.s fa0, fa1, fa2   # encoding: [0x53,0x95,0xc5,0x20]
        fsgnjx.s fa0, fa1, fa2
# CHECK: fsgnjx.s fa0, fa1, fa2   # encoding: [0x53,0xa5,0xc5,0x20]

        fmin.s  fa0, fa1, fa2
# CHECK: fmin.s fa0, fa1, fa2     # encoding: [0x53,0x85,0xc5,0x28]
        fmax.s  fa0, fa1, fa2
# CHECK: fmax.s fa0, fa1, fa2     # encoding: [0x53,0x95,0xc5,0x28]

        fcvt.w.s  a0, fa1
# CHECK: fcvt.w.s a0, fa1         # encoding: [0x53,0xf5,0x05,0xc0]
        fcvt.wu.s a0, fa1
# CHECK: fcvt.wu.s a0, fa1        # encoding: [0x53,0xf5,0x15,0xc0]
        fcvt.s.w  fa0, a1
# CHECK: fcvt.s.w fa0, a1         # encoding: [0x53,0xf5,0x05,0xd0]
        fcvt.s.wu fa0, a1
# CHECK: fcvt.s.wu fa0, a1        # encoding: [0x53,0xf5,0x15,0xd0]
        fcvt.w.s  a0, fa1, rtz
# CHECK: fcvt.w.s a0, fa1, rtz    # encoding: [0x53,0x95,0x05,0xc0]
        fcvt.wu.s a0, fa1, rtz
# CHECK: fcvt.wu.s a0, fa1, rtz   # encoding: [0x53,0x95,0x15,0xc0]

        fmv.x.w a0, fa1
# CHECK: fmv.x.w a0, fa1          # encoding: [0x53,0x85,0x05,0xe0]
        fmv.w.x fa0, a1
# CHECK: fmv.w.x fa0, a1          # encoding: [0x53,0x85,0x05,0xf0]

        feq.s   a0, fa1, fa2
# CHECK: feq.s a0, fa1, fa2       # encoding: [0x53,0xa5,0xc5,0xa0]
        flt.s   a0, fa1, fa2
# CHECK: flt.s a0, fa1, fa2       # encoding: [0x53,0x95,0xc5,0xa0]
        fle.s   a0, fa1, fa2
# CHECK: fle.s a0, fa1, fa2       # encoding: [0x53,0x85,0xc5,0xa0]
        fclass.s a0, fa1
# CHECK: fclass.s a0, fa1         # encoding: [0x53,0x95,0x05,0xe0]

# MISSING: error: instruction requires the following: 'F' (Single-Precision Floating-Point)

# The disassembler prints the same explicit rounding-mode form, and `fabs` /
# `fmv.s` are just aliases for the sign-injection encodings.
# DIS:       flw {{fa[0-9]+}}, 0x0(a0)
# DIS:       fadd.s {{fa[0-9]+}}, fa1, fa2
# DIS:       fcvt.w.s a0, fa1, rtz
# DIS:       fcvt.wu.s a0, fa1, rtz
# DIS:       fclass.s a0, fa1
