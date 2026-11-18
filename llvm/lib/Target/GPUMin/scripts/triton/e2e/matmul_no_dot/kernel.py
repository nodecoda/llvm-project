"""Chapter 27 e2e: 32x32x32 f32 matmul without tl.dot.

Every block owns one output row: program_id is the row, and the 32 lanes
cover the columns.  The k loop is unrolled at compile time, so the kernel
only needs scalar loads for A, vector loads for B and one fadd/fmul pair
per step.  A[r][k] = r*32+k and B[k][c] = 1, therefore
C[r][c] = sum_k (r*32+k) = 1024*r + 496.
"""

import triton
import triton.language as tl


@triton.jit
def matmul_kernel(a_ptr, b_ptr, c_ptr):
    row = tl.program_id(0)
    col = tl.arange(0, 32)
    acc = 0.0
    for k in tl.static_range(0, 32):
        a = tl.load(a_ptr + row * 32 + k)
        b = tl.load(b_ptr + k * 32 + col)
        acc = acc + a * b
    tl.store(c_ptr + row * 32 + col, acc)
