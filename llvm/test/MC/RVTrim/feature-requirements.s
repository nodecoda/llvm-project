# RUN: not llvm-mc -triple=rvtrim64 -filetype=obj %s -o /dev/null 2>&1 \
# RUN:   | FileCheck %s --check-prefix=NO-M
# RUN: llvm-mc -triple=rvtrim64 -mattr=+m -show-encoding %s \
# RUN:   | FileCheck %s --check-prefix=M

# The M instructions stay behind their predicates even though the records are
# generated: rvtrim64 alone has no 'm', so the asm parser must ask for Zmmul.
# NO-M: error: instruction requires the following: 'Zmmul' (Integer Multiplication)
# M: mul a0, a1, a2{{.*}}# encoding: [0x33,0x85,0xc5,0x02]
mul a0, a1, a2
