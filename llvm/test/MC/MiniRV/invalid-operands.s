# RUN: not llvm-mc -triple=minirv64 %s -o /dev/null 2>&1 | FileCheck %s

# The immediate predicates and the operand classes come from
# MiniRVInstrInfo.td / the auto-td taxonomy, so the diagnostics below are the
# ones the generated matcher selects.

# CHECK: error: operand must be a 12-bit signed immediate
addi x1, x2, 4096

# CHECK: error: operand must be a 12-bit signed immediate
addi x1, x2, -2049

# CHECK: error: operand must be a 20-bit unsigned immediate
lui x1, 1048576

# CHECK: error: too few operands for instruction
add x1, x2

# CHECK: error: invalid operand for instruction
add x1, x2, x3, x4

# CHECK: error: invalid instruction mnemonic
frob x1, x2
