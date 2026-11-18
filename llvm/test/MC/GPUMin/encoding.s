# RUN: llvm-mc -triple=gpumin -show-encoding < %s | FileCheck %s
# RUN: llvm-mc -triple=gpumin -filetype=obj < %s -o %t.o
# RUN: llvm-objdump -d -r %t.o | FileCheck %s --check-prefix=DIS

# Chapter 10 static corpus: immediates, memory, branches, calls and specials.

# CHECK: addi r1, r2, -16       # encoding: [0xf0,0xff,0x22,0x18]
addi r1, r2, -16

# CHECK: ld.global.b32 r7, 8(r8)   # encoding: [0x04,0x01,0xe8,0x20]
# CHECK: st.global.b32 r9, 4(r10)  # encoding: [0x84,0x00,0x2a,0x25]
# CHECK: ld.global.u8 r1, 0(r2)    # encoding: [0x00,0x00,0x22,0x20]
# CHECK: ld.global.s8 r1, 0(r2)    # encoding: [0x01,0x00,0x22,0x20]
# CHECK: st.global.b8 r1, 0(r2)    # encoding: [0x00,0x00,0x22,0x24]
ld.global.b32 r7, 8(r8)
st.global.b32 r9, 4(r10)
ld.global.u8 r1, 0(r2)
ld.global.s8 r1, 0(r2)
st.global.b8 r1, 0(r2)

# CHECK: sr.laneid r1          # encoding: [0x02,0x00,0x20,0x48]
# CHECK: sr.activemask r1      # encoding: [0x08,0x00,0x20,0x48]
sr.laneid r1
sr.activemask r1

# Branch targets are fixups, so check them after assembly.
# DIS: bra 2
# DIS: bra p1, 0
bra .L1
exit
.L1:
bra p1, .L1

# A local call is a word displacement.
# DIS: call 2
# DIS: exit
call .Lcall
exit
.Lcall:
exit
