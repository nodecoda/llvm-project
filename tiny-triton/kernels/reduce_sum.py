# Tiny Triton chapter 6: one block reduces 1024 f32 values to one scalar.
# The grid is one block, so the scalar store needs no program-id offset.
import triton
import triton.language as tl


@triton.jit
def reduce_sum(x_ptr, out_ptr):
    offs = tl.arange(0, 1024)
    x = tl.load(x_ptr + offs)
    total = tl.sum(x, axis=0)
    tl.store(out_ptr, total)
