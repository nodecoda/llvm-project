"""Chapter 29 e2e: an 8x8 2D copy.

The full sample (transpose, broadcast and 2D grid) lives in
``../../layout2d.py``; this program is the smallest end-to-end check that the
4x8 thread layout, the 2D pointer tensor and the 2D load/store agree.
"""

import triton
import triton.language as tl


@triton.jit
def copy2d_kernel(in_ptr, out_ptr):
    r = tl.arange(0, 8)[:, None]
    c = tl.arange(0, 8)[None, :]
    offs = r * 8 + c
    x = tl.load(in_ptr + offs)
    tl.store(out_ptr + offs, x)
