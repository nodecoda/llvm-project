"""Chapter 31 sample: row-wise softmax over an 8x8 f32 tile.

The block owns the whole tile (4 warps x 8 lanes, two elements per thread).
Both reductions run along axis 1: the row maximum first, then the sum of the
exponentials.  keep_dims=True keeps the [8,1] shape so the result broadcasts
straight back over [8,8].  exp uses exp2 with the 1/ln(2) rescale, which is
the one transcendental the GPUMin SFU provides.
"""

import triton
import triton.language as tl


@triton.jit
def softmax_kernel(x_ptr):
    r = tl.arange(0, 8)[:, None]
    c = tl.arange(0, 8)[None, :]
    x = tl.load(x_ptr + r * 8 + c)
    m = tl.max(x, axis=1, keep_dims=True)
    e = tl.exp2((x - m) * 1.4426950408889634)
    s = tl.sum(e, axis=1, keep_dims=True)
    y = e / s
    tl.store(x_ptr + r * 8 + c, y)
