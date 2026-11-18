"""Chapter 27 sample: tl.where and compile-time loops.

The C++ frontend lowers ``tl.where`` to ``arith.select`` and unrolls both
``tl.static_range`` and ``range`` because every loop bound is a compile-time
integer.  The chapter 27 gate runs the first three kernels on the simulator
and checks that the unrolled accumulator is 16.
"""

import triton
import triton.language as tl


@triton.jit
def where_kernel(x_ptr, y_ptr, out_ptr, BLOCK: tl.constexpr):
    offs = tl.arange(0, BLOCK)
    x = tl.load(x_ptr + offs)
    y = tl.load(y_ptr + offs)
    z = tl.where(x < y, x, y)
    tl.store(out_ptr + offs, z)


@triton.jit
def fwhere_kernel(fx_ptr, fy_ptr, fout_ptr, BLOCK: tl.constexpr):
    offs = tl.arange(0, BLOCK)
    x = tl.load(fx_ptr + offs)
    y = tl.load(fy_ptr + offs)
    z = tl.where(x < y, x, y)
    tl.store(fout_ptr + offs, z)


@triton.jit
def swhere_kernel(sout_ptr):
    pid = tl.program_id(0)
    v = tl.where(pid > 0, 1.5, 2.5)
    tl.store(sout_ptr + pid, v)


@triton.jit
def unroll_kernel(uout_ptr):
    acc = 0
    for i in tl.static_range(0, 5):
        acc = acc + i
    for j in range(1, 4):
        acc = acc + j
    tl.store(uout_ptr, acc)
