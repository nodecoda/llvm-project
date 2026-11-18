# RUN: llvm-mc -triple=gpumin -show-encoding < %s | FileCheck %s
# RUN: llvm-mc -triple=gpumin -filetype=obj < %s -o %t.o
# RUN: llvm-objdump -d %t.o | FileCheck %s --check-prefix=DIS

# Chapter 09: special-register reads.  Each selector is its own funct value in
# the SYS opcode, so the mnemonic and the encoding are one-to-one.

# CHECK: sr.laneid r1          # encoding: [0x02,0x00,0x20,0x48]
# CHECK: sr.warpid r1          # encoding: [0x03,0x00,0x20,0x48]
# CHECK: sr.blockid r1         # encoding: [0x04,0x00,0x20,0x48]
# CHECK: sr.tid r1             # encoding: [0x05,0x00,0x20,0x48]
# CHECK: sr.ntid r1            # encoding: [0x06,0x00,0x20,0x48]
# CHECK: sr.nwarps r1          # encoding: [0x07,0x00,0x20,0x48]
# CHECK: sr.activemask r1      # encoding: [0x08,0x00,0x20,0x48]
sr.laneid r1
sr.warpid r1
sr.blockid r1
sr.tid r1
sr.ntid r1
sr.nwarps r1
sr.activemask r1

# The destination is the ordinary 5-bit GPR field.
# CHECK: sr.laneid r7          # encoding: [0x02,0x00,0xe0,0x48]
# CHECK: sr.activemask r31     # encoding: [0x08,0x00,0xe0,0x4b]
sr.laneid r7
sr.activemask r31

# DIS-LABEL: <{{.*}}>:
# DIS: sr.laneid r1
# DIS: sr.warpid r1
# DIS: sr.blockid r1
# DIS: sr.tid r1
# DIS: sr.ntid r1
# DIS: sr.nwarps r1
# DIS: sr.activemask r1
# DIS: sr.laneid r7
# DIS: sr.activemask r31
