# RUN: llvm-mc -triple=gpumin -show-encoding < %s | FileCheck %s
# RUN: llvm-mc -triple=gpumin -filetype=obj < %s -o %t.o
# RUN: llvm-objdump -d %t.o | FileCheck %s --check-prefix=DIS

# Chapter 10 static corpus: data movement, ALU, predicates and returns.

# CHECK: mov r1, r2             # encoding: [0x00,0x00,0x22,0x04]
# CHECK: movi r3, 42            # encoding: [0x2a,0x00,0x60,0x14]
mov r1, r2
movi r3, 42

# CHECK: add r4, r5, r6         # encoding: [0x00,0x30,0x85,0x08]
# CHECK: sub r7, r8, r9         # encoding: [0x01,0x48,0xe8,0x08]
# CHECK: and r10, r11, r12      # encoding: [0x02,0x60,0x4b,0x09]
# CHECK: or r13, r14, r15       # encoding: [0x03,0x78,0xae,0x09]
# CHECK: xor r16, r17, r18      # encoding: [0x04,0x90,0x11,0x0a]
# CHECK: shl r19, r20, r21      # encoding: [0x05,0xa8,0x74,0x0a]
# CHECK: shr r22, r23, r24      # encoding: [0x06,0xc0,0xd7,0x0a]
# CHECK: sar r25, r26, r27      # encoding: [0x07,0xd8,0x3a,0x0b]
# Chapter 28: extended ALU funct space (funct[10:4]).
# CHECK: sdiv r4, r5, r6        # encoding: [0x10,0x30,0x85,0x08]
# CHECK: udiv r7, r8, r9        # encoding: [0x11,0x48,0xe8,0x08]
# CHECK: srem r10, r11, r12     # encoding: [0x12,0x60,0x4b,0x09]
# CHECK: urem r13, r14, r15     # encoding: [0x13,0x78,0xae,0x09]
# CHECK: mul r16, r17, r18      # encoding: [0x14,0x90,0x11,0x0a]
add r4, r5, r6
sub r7, r8, r9
and r10, r11, r12
or r13, r14, r15
xor r16, r17, r18
shl r19, r20, r21
shr r22, r23, r24
sar r25, r26, r27
sdiv r4, r5, r6
udiv r7, r8, r9
srem r10, r11, r12
urem r13, r14, r15
mul r16, r17, r18

# CHECK: setp.eq p0, r1, r2     # encoding: [0x00,0x10,0x01,0x0c]
# CHECK: setp.lt p7, r30, r31   # encoding: [0x02,0xf8,0xfe,0x0c]
# CHECK: selp r4, p0, r4, r5    # encoding: [0x00,0x28,0x84,0x10]
setp.eq p0, r1, r2
setp.lt p7, r30, r31
selp r4, p0, r4, r5

# CHECK: ret                   # encoding: [0x01,0x00,0x00,0x48]
# CHECK: exit                  # encoding: [0x00,0x00,0x00,0x48]
ret
exit

# DIS-LABEL: <{{.*}}>:
# DIS: mov r1, r2
# DIS: movi r3, 42
# DIS: add r4, r5, r6
# DIS: sub r7, r8, r9
# DIS: and r10, r11, r12
# DIS: or r13, r14, r15
# DIS: xor r16, r17, r18
# DIS: shl r19, r20, r21
# DIS: shr r22, r23, r24
# DIS: sar r25, r26, r27
# DIS: sdiv r4, r5, r6
# DIS: udiv r7, r8, r9
# DIS: srem r10, r11, r12
# DIS: urem r13, r14, r15
# DIS: mul r16, r17, r18
# DIS: setp.eq p0, r1, r2
# DIS: setp.lt p7, r30, r31
# DIS: selp r4, p0, r4, r5
# DIS: ret
# DIS: exit
