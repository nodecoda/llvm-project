"""Chapter 35 sample: chapter 30's matmul expressed as one tl.dot.

Each block still owns one 8x8 tile of C and loads the same A strip (8x32)
and B strip (32x8).  The difference is that the 32-step rank-1 loop is now a
single tl.dot: the TTIR prints a real tt.dot with inputPrecision = tf32, and
the backend expands it into shared staging plus the same f32 rank-1 update.
The launch descriptor and reference values are chapter 30's, so the result
must be bit-identical.
"""

import triton
import triton.language as tl


@triton.jit
def dot_kernel(a_ptr, b_ptr, c_ptr):
    pm = tl.program_id(0)
    pn = tl.program_id(1)
    rm = pm * 8 + tl.arange(0, 8)[:, None]
    rn = pn * 8 + tl.arange(0, 8)[None, :]
    kk_r = tl.arange(0, 32)[None, :]
    kk_c = tl.arange(0, 32)[:, None]

    a = tl.load(a_ptr + rm * 32 + kk_r)
    b = tl.load(b_ptr + kk_c * 32 + rn)
    acc = tl.dot(a, b)
    tl.store(c_ptr + rm * 32 + rn, acc)
