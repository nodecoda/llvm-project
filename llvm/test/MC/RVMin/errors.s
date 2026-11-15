# RUN: not llvm-mc -triple=rvmin %s -o /dev/null 2>&1 | FileCheck %s
#
# Immediate ranges are enforced by the operand predicates in
# RVMinInstrInfo.td; each case here is one field boundary.

# CHECK: error: invalid operand for instruction
        addi    a0, a1, 4096
# CHECK: error: invalid operand for instruction
        sw      a0, 4096(sp)
# CHECK: error: invalid operand for instruction
        lui     a0, -1
# CHECK: error: invalid operand for instruction
        beq     a0, a1, 9
# CHECK: error: invalid operand for instruction
        slli    a0, a1, 32
