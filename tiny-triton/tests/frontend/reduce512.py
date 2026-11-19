# Chapter 6 regression: a block reduce has 32 partials even when every thread
# holds only 16 elements.  The lowering must read all 32 shared slots.
import triton
import triton.language as tl


@triton.jit
def reduce512(x_ptr, out_ptr):
    offs = tl.arange(0, 512)
    x = tl.load(x_ptr + offs)
    total = tl.sum(x, axis=0)
    tl.store(out_ptr, total)
