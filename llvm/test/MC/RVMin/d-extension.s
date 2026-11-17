# RUN: llvm-mc -triple=rvmin -mattr=+f,+d -show-encoding %s | FileCheck %s
# RUN: llvm-mc -triple=rvmin -mattr=+d -show-encoding %s | FileCheck %s
# RUN: llvm-mc -triple=rvmin -mcpu=generic-rv32-d -show-encoding %s | FileCheck %s
# RUN: not llvm-mc -triple=rvmin -filetype=obj %s -o /dev/null 2>&1 \
# RUN:   | FileCheck %s --check-prefix=MISSING
# RUN: llvm-mc -triple=rvmin -mattr=+f,+d -filetype=obj %s -o %t.o
# RUN: llvm-objdump -d --mattr=+f,+d %t.o | FileCheck %s --check-prefix=DIS
#
# Chapter 41: the D extension.  Every encoding below is byte-identical to the
# reference RISC-V assembler (`llvm-mc -triple=riscv32 -mattr=+d`), and
# scripts/check-rvmin-encodings.sh re-checks that on every run.
#
# Two encoding details are easy to get wrong:
#
#   * `fcvt.d.s` and `fcvt.d.w` are exact conversions, so the ISA encodes them
#     with rm = 000 (round to nearest, ties to even), not the dynamic mode.
#   * The `rtz` in `fcvt.w.d ..., rtz` is a literal token operand, exactly
#     like the F chapter's `fcvt.w.s ..., rtz`.

        fld fa0, 0(a0)                  
# CHECK: fld fa0, 0(a0)                         # encoding: [0x07,0x35,0x05,0x00]

        fsd fa1, 4(a1)                  
# CHECK: fsd fa1, 4(a1)                         # encoding: [0x27,0xb2,0xb5,0x00]

        fmadd.d fa0, fa1, fa2, fa3      
# CHECK: fmadd.d fa0, fa1, fa2, fa3             # encoding: [0x43,0xf5,0xc5,0x6a]

        fmsub.d fa0, fa1, fa2, fa3      
# CHECK: fmsub.d fa0, fa1, fa2, fa3             # encoding: [0x47,0xf5,0xc5,0x6a]

        fnmsub.d fa0, fa1, fa2, fa3     
# CHECK: fnmsub.d fa0, fa1, fa2, fa3            # encoding: [0x4b,0xf5,0xc5,0x6a]

        fnmadd.d fa0, fa1, fa2, fa3     
# CHECK: fnmadd.d fa0, fa1, fa2, fa3            # encoding: [0x4f,0xf5,0xc5,0x6a]

        fadd.d fa0, fa1, fa2            
# CHECK: fadd.d fa0, fa1, fa2                   # encoding: [0x53,0xf5,0xc5,0x02]

        fsub.d fa0, fa1, fa2            
# CHECK: fsub.d fa0, fa1, fa2                   # encoding: [0x53,0xf5,0xc5,0x0a]

        fmul.d fa0, fa1, fa2            
# CHECK: fmul.d fa0, fa1, fa2                   # encoding: [0x53,0xf5,0xc5,0x12]

        fdiv.d fa0, fa1, fa2            
# CHECK: fdiv.d fa0, fa1, fa2                   # encoding: [0x53,0xf5,0xc5,0x1a]

        fsqrt.d fa0, fa1                
# CHECK: fsqrt.d fa0, fa1                       # encoding: [0x53,0xf5,0x05,0x5a]

        fsgnj.d fa0, fa1, fa2           
# CHECK: fsgnj.d fa0, fa1, fa2                  # encoding: [0x53,0x85,0xc5,0x22]

        fsgnjn.d fa0, fa1, fa2          
# CHECK: fsgnjn.d fa0, fa1, fa2                 # encoding: [0x53,0x95,0xc5,0x22]

        fsgnjx.d fa0, fa1, fa2          
# CHECK: fsgnjx.d fa0, fa1, fa2                 # encoding: [0x53,0xa5,0xc5,0x22]

        fmin.d fa0, fa1, fa2            
# CHECK: fmin.d fa0, fa1, fa2                   # encoding: [0x53,0x85,0xc5,0x2a]

        fmax.d fa0, fa1, fa2            
# CHECK: fmax.d fa0, fa1, fa2                   # encoding: [0x53,0x95,0xc5,0x2a]

        fcvt.w.d a0, fa1                
# CHECK: fcvt.w.d a0, fa1                       # encoding: [0x53,0xf5,0x05,0xc2]

        fcvt.wu.d a0, fa1               
# CHECK: fcvt.wu.d a0, fa1                      # encoding: [0x53,0xf5,0x15,0xc2]

        fcvt.d.w fa0, a1                
# CHECK: fcvt.d.w fa0, a1                       # encoding: [0x53,0x85,0x05,0xd2]

        fcvt.d.wu fa0, a1               
# CHECK: fcvt.d.wu fa0, a1                      # encoding: [0x53,0x85,0x15,0xd2]

        fcvt.w.d a0, fa1, rtz           
# CHECK: fcvt.w.d a0, fa1, rtz                  # encoding: [0x53,0x95,0x05,0xc2]

        fcvt.wu.d a0, fa1, rtz          
# CHECK: fcvt.wu.d a0, fa1, rtz                 # encoding: [0x53,0x95,0x15,0xc2]

        fcvt.s.d fa0, fa1               
# CHECK: fcvt.s.d fa0, fa1                      # encoding: [0x53,0xf5,0x15,0x40]

        fcvt.d.s fa0, fa1               
# CHECK: fcvt.d.s fa0, fa1                      # encoding: [0x53,0x85,0x05,0x42]

        feq.d a0, fa1, fa2              
# CHECK: feq.d a0, fa1, fa2                     # encoding: [0x53,0xa5,0xc5,0xa2]

        flt.d a0, fa1, fa2              
# CHECK: flt.d a0, fa1, fa2                     # encoding: [0x53,0x95,0xc5,0xa2]

        fle.d a0, fa1, fa2              
# CHECK: fle.d a0, fa1, fa2                     # encoding: [0x53,0x85,0xc5,0xa2]

        fclass.d a0, fa1                
# CHECK: fclass.d a0, fa1                       # encoding: [0x53,0x95,0x05,0xe2]

# MISSING: error: instruction requires the following: 'D' (Double-Precision Floating-Point)

# The disassembler prints the same explicit rounding-mode form.
# DIS:       fld {{fa[0-9]+}}, 0x0(a0)
# DIS:       fadd.d {{fa[0-9]+}}, fa1, fa2
# DIS:       fcvt.w.d a0, fa1, rtz
# DIS:       fcvt.wu.d a0, fa1, rtz
# DIS:       fclass.d a0, fa1
