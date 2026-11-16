# RUN: split-file %s %t
# RUN: llvm-mc -triple=rvmin -mattr=+zbb,+zbs -show-encoding %t/all.s | FileCheck %s
# RUN: not llvm-mc -triple=rvmin -filetype=obj %t/all.s -o /dev/null 2>&1 | FileCheck %s --check-prefix=NOZBB
# RUN: not llvm-mc -triple=rvmin -mattr=+zbb -filetype=obj %t/all.s -o /dev/null 2>&1 | FileCheck %s --check-prefix=NOZBS
#
# Chapter 21: Zbb/Zbs encodings, checked against the reference RISC-V
# assembler by scripts/check-rvmin-encodings.sh.  Here the bytes are pinned so
# a TableGen change cannot silently move them.

#--- all.s
clz a0, a1
ctz a0, a1
cpop a0, a1
sext.b a0, a1
sext.h a0, a1
rev8 a0, a1
zext.h a0, a1
rol a0, a1, a2
ror a0, a1, a2
rori a0, a1, 5
andn a0, a1, a2
orn a0, a1, a2
xnor a0, a1, a2
min a0, a1, a2
minu a0, a1, a2
max a0, a1, a2
maxu a0, a1, a2
bset a0, a1, a2
bclr a0, a1, a2
binv a0, a1, a2
bext a0, a1, a2
bseti a0, a1, 5
bclri a0, a1, 5
binvi a0, a1, 5
bexti a0, a1, 5

# CHECK: clz a0, a1                     # encoding: [0x13,0x95,0x05,0x60]
# CHECK: ctz a0, a1                     # encoding: [0x13,0x95,0x15,0x60]
# CHECK: cpop a0, a1                    # encoding: [0x13,0x95,0x25,0x60]
# CHECK: sext.b a0, a1                  # encoding: [0x13,0x95,0x45,0x60]
# CHECK: sext.h a0, a1                  # encoding: [0x13,0x95,0x55,0x60]
# CHECK: rev8 a0, a1                    # encoding: [0x13,0xd5,0x85,0x69]
# CHECK: zext.h a0, a1                  # encoding: [0x33,0xc5,0x05,0x08]
# CHECK: rol a0, a1, a2                 # encoding: [0x33,0x95,0xc5,0x60]
# CHECK: ror a0, a1, a2                 # encoding: [0x33,0xd5,0xc5,0x60]
# CHECK: rori a0, a1, 5                 # encoding: [0x13,0xd5,0x55,0x60]
# CHECK: andn a0, a1, a2                # encoding: [0x33,0xf5,0xc5,0x40]
# CHECK: orn a0, a1, a2                 # encoding: [0x33,0xe5,0xc5,0x40]
# CHECK: xnor a0, a1, a2                # encoding: [0x33,0xc5,0xc5,0x40]
# CHECK: min a0, a1, a2                 # encoding: [0x33,0xc5,0xc5,0x0a]
# CHECK: minu a0, a1, a2                # encoding: [0x33,0xd5,0xc5,0x0a]
# CHECK: max a0, a1, a2                 # encoding: [0x33,0xe5,0xc5,0x0a]
# CHECK: maxu a0, a1, a2                # encoding: [0x33,0xf5,0xc5,0x0a]
# CHECK: bset a0, a1, a2                # encoding: [0x33,0x95,0xc5,0x28]
# CHECK: bclr a0, a1, a2                # encoding: [0x33,0x95,0xc5,0x48]
# CHECK: binv a0, a1, a2                # encoding: [0x33,0x95,0xc5,0x68]
# CHECK: bext a0, a1, a2                # encoding: [0x33,0xd5,0xc5,0x48]
# CHECK: bseti a0, a1, 5                # encoding: [0x13,0x95,0x55,0x28]
# CHECK: bclri a0, a1, 5                # encoding: [0x13,0x95,0x55,0x48]
# CHECK: binvi a0, a1, 5                # encoding: [0x13,0x95,0x55,0x68]
# CHECK: bexti a0, a1, 5                # encoding: [0x13,0xd5,0x55,0x48]

# NOZBB: error: instruction requires the following: 'Zbb' (Basic Bit-Manipulation)
# NOZBS: error: instruction requires the following: 'Zbs' (Single-Bit Instructions)
