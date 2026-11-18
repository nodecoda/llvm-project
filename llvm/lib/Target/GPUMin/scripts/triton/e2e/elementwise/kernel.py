"""Chapter 28 e2e: Python floor division, cdiv, full/zeros and min/max.

Only ``alu_kernel`` is needed here; the full elementwise sample (f32
maximum/minimum and f32 tl.max) lives in ../elementwise.py and is covered by
the chapter-28 gate and the Triton harness.
"""

import triton
import triton.language as tl


@triton.jit
def alu_kernel(out_ptr, BLOCK: tl.constexpr):
    offs = tl.arange(0, BLOCK)
    a = offs - 16
    b = 3
    q = a // b
    r = a % b
    c = tl.cdiv(a, b)
    hi = tl.maximum(a, 0)
    lo = tl.minimum(0, a)
    z = tl.zeros((BLOCK,), tl.int32)
    f = tl.full((BLOCK,), 7, tl.int32)
    tl.store(out_ptr + offs, q + r + c + hi + lo + z + f)
