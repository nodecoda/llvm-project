"""Chapter 30 sample: a 32x32x32 f32 matmul without tl.dot.

Each block owns one 8x8 tile of C.  The K loop is unrolled at compile time and
every iteration is a rank-1 update: load a column of A and a row of B, then
broadcast their outer product over the tile.  The 8x8 tile is exactly the
chapter-29 layout (4 warps x 8 lanes, two elements per thread).
"""

import triton
import triton.language as tl


@triton.jit
def matmul2d_kernel(a_ptr, b_ptr, c_ptr):
    pm = tl.program_id(0)
    pn = tl.program_id(1)
    rm = pm * 8 + tl.arange(0, 8)[:, None]
    rn = pn * 8 + tl.arange(0, 8)[None, :]
    acc = tl.zeros((8, 8), tl.float32)
    for k in tl.static_range(0, 32):
        a = tl.load(a_ptr + rm * 32 + k)
        b = tl.load(b_ptr + k * 32 + rn)
        acc = acc + a * b
    tl.store(c_ptr + rm * 32 + rn, acc)
