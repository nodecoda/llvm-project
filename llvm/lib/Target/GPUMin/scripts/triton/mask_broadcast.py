"""Chapter 36 sample: a row mask broadcast over a 2D pointer tile.

``colmask`` is [1,8]; the pointer tile is [8,8].  The load broadcasts the mask
up to the pointer shape and fills the four masked columns with ``other=0.0``.
The pointer shape never grows, so the 4x8 thread layout is untouched.
"""

import triton
import triton.language as tl


@triton.jit
def mask_broadcast_kernel(x_ptr, out_ptr):
    r = tl.arange(0, 8)[:, None]
    c = tl.arange(0, 8)[None, :]
    colmask = c < 4
    x = tl.load(x_ptr + r * 8 + c, mask=colmask, other=0.0)
    tl.store(out_ptr + r * 8 + c, x)
