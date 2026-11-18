"""Chapter 29 sample: 2D tensors, 2D grids and layout conversion.

Four kernels cover the chapter surface:

* ``copy2d_kernel`` builds an 8x8 tile from ``arange(0,8)[:, None]`` and
  ``arange(0,8)[None, :]`` and copies it through a 2D pointer tensor;
* ``trans2d_kernel`` transposes the tile with ``tl.trans`` (shared memory and
  two barriers in the backend);
* ``bcast2d_kernel`` broadcasts a row vector and a column vector against each
  other: ``out[r, c] = r + 10 * c``;
* ``grid2d_kernel`` runs on a 2D grid and uses ``tl.program_id(0/1)`` in the
  address computation.

The 4x8 thread grid gives every lane two elements: (2*warpid, laneid) and
(2*warpid+1, laneid).
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


@triton.jit
def trans2d_kernel(in_ptr, out_ptr):
    r = tl.arange(0, 8)[:, None]
    c = tl.arange(0, 8)[None, :]
    offs = r * 8 + c
    x = tl.load(in_ptr + offs)
    y = tl.trans(x)
    tl.store(out_ptr + offs, y)


@triton.jit
def bcast2d_kernel(out_ptr):
    r = tl.arange(0, 8)[:, None]
    c = tl.arange(0, 8)[None, :]
    v = r + 10 * c
    tl.store(out_ptr + r * 8 + c, v)


@triton.jit
def grid2d_kernel(in_ptr, out_ptr):
    bx = tl.program_id(0)
    by = tl.program_id(1)
    r = tl.arange(0, 8)[:, None]
    c = tl.arange(0, 8)[None, :]
    offs = (by * 8 + r) * 16 + bx * 8 + c
    x = tl.load(in_ptr + offs)
    tl.store(out_ptr + offs, x + bx + by)
