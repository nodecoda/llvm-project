# RUN: split-file %s %t
# RUN: llvm-mc -triple=rvmin -disassemble %t/rv32i.txt | FileCheck %s --check-prefix=RV32I
# RUN: llvm-mc -triple=rvmin -mattr=+m -disassemble %t/m.txt | FileCheck %s --check-prefix=M
# RUN: llvm-mc -triple=rvmin -disassemble %t/m.txt 2>&1 | FileCheck %s --check-prefix=NOM
# RUN: llvm-mc -triple=rvmin -disassemble %t/bad.txt 2>&1 | FileCheck %s --check-prefix=BAD
# RUN: llvm-mc -triple=rvmin -disassemble %t/short.txt 2>&1 | FileCheck %s --check-prefix=SHORT
#
# Chapter 20: the disassembler is generated from the same instruction
# definitions as the encoder.  The interesting cases are the signed
# immediates: I- and S-type values are sign-extended, while B- and J-type
# offsets arrive as scattered instruction bits but print as full byte offsets.
#
# The negative runs check that (a) M instructions are rejected without +m and
# (b) a trailing word that is not four bytes long cannot be decoded.

#--- rv32i.txt
# R-type
0x33 0x85 0xc5 0x00
# I-type, negative immediate
0x13 0x85 0xc5 0xff
# S-type
0x23 0x24 0xa1 0x00
# B-type, backward target
0xe3 0x1c 0xb5 0xfe
# J-type
0x6f 0x00 0x00 0x01
# U-type
0x37 0x55 0x34 0x12
# JALR with a negative offset
0xe7 0x00 0xc1 0xff
# Shift with funct7 = 0b0100000
0x13 0xd5 0xf5 0x41
# SYSTEM
0x73 0x00 0x00 0x00
0x73 0x00 0x10 0x00

# RV32I: {{^}}	add a0, a1, a2{{$}}
# RV32I-NEXT: {{^}}	addi a0, a1, -4{{$}}
# RV32I-NEXT: {{^}}	sw a0, 8(sp){{$}}
# RV32I-NEXT: {{^}}	bne a0, a1, -8{{$}}
# RV32I-NEXT: {{^}}	jal zero, 16{{$}}
# RV32I-NEXT: {{^}}	lui a0, 74565{{$}}
# RV32I-NEXT: {{^}}	jalr ra, -4(sp){{$}}
# RV32I-NEXT: {{^}}	srai a0, a1, 31{{$}}
# RV32I-NEXT: {{^}}	ecall{{$}}
# RV32I-NEXT: {{^}}	ebreak{{$}}

#--- m.txt
0x33 0x85 0xc5 0x02

# M: {{^}}	mul a0, a1, a2{{$}}
#
# Without +m the decoder table must not match; the word is reported as invalid
# instead of being silently decoded as an RV32I instruction.
# NOM: warning: invalid instruction encoding

#--- bad.txt
0xff 0xff 0xff 0xff

# BAD: warning: invalid instruction encoding

#--- short.txt
0x13 0x05

# SHORT: warning: invalid instruction encoding
