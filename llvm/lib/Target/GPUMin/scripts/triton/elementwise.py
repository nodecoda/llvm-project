"""Chapter 28 sample: elementwise completeness and broadcasting.

The three kernels cover the chapter's new surface:

* ``alu_kernel`` uses Python ``//``/``%`` (floor semantics, not C's
  truncation), ``tl.cdiv``, ``tl.maximum``/``tl.minimum`` with scalar
  broadcasting, and ``tl.zeros``/``tl.full``;
* ``fminmax_kernel`` does the same maximum/minimum on f32, which lowers to
  ``cmpf ogt/olt`` plus ``arith.select``;
* ``fmax_kernel`` reduces an f32 tensor with ``tl.max``; the backend combines
  it with an ordered compare and SELP.

The chapter 28 gate compares the first kernel element by element against a
Python floor-division reference and checks the f32 reduction bit patterns.
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


@triton.jit
def fminmax_kernel(fx_ptr, fy_ptr, fout_ptr, BLOCK: tl.constexpr):
    offs = tl.arange(0, BLOCK)
    x = tl.load(fx_ptr + offs)
    y = tl.load(fy_ptr + offs)
    tl.store(fout_ptr + offs, tl.maximum(x, y) - tl.minimum(x, y))


@triton.jit
def fmax_kernel(fx_ptr, fout_ptr, BLOCK: tl.constexpr):
    offs = tl.arange(0, BLOCK)
    x = tl.load(fx_ptr + offs)
    m = tl.max(x, axis=0)
    tl.store(fout_ptr, m)
