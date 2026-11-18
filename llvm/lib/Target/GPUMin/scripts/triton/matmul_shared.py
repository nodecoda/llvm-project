"""Chapter 34 sample: the chapter-30 matmul with shared-memory staging.

Each block still owns one 8x8 tile of C, but it first stages its A strip
(8x32) and B strip (32x8) in shared memory.  The rank-1 K loop then reads
the strips through tl.load_shared_col/row instead of reloading each column
and row from global memory four times (once per warp).

The launch descriptor and the reference values are identical to
e2e/matmul2d, so chapter 34 can assert bit-exact agreement with chapter 30
and compare glb_ld on the same input.
"""

import triton
import triton.language as tl


@triton.jit
def matmul_shared_kernel(a_ptr, b_ptr, c_ptr):
    pm = tl.program_id(0)
    pn = tl.program_id(1)
    rm = pm * 8 + tl.arange(0, 8)[:, None]
    rn = pn * 8 + tl.arange(0, 8)[None, :]
    kk_r = tl.arange(0, 32)[None, :]
    kk_c = tl.arange(0, 32)[:, None]

    sa = tl.alloc_shared((8, 32), tl.float32)
    sb = tl.alloc_shared((32, 8), tl.float32)

    # Stage one strip at a time: both live at once costs 16 registers per
    # thread and spills.  This order keeps at most eight live.
    a_tile = tl.load(a_ptr + rm * 32 + kk_r)
    tl.store_shared(sa, a_tile)
    b_tile = tl.load(b_ptr + kk_c * 32 + rn)
    tl.store_shared(sb, b_tile)

    acc = tl.zeros((8, 8), tl.float32)
    for k in tl.static_range(0, 32):
        acc = acc + tl.load_shared_col(sa, k) * tl.load_shared_row(sb, k)
    tl.store(c_ptr + rm * 32 + rn, acc)
