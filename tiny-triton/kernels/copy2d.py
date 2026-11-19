# Tiny Triton frontend input (chapter 7a).  A 2-D index tile built from a
# column vector and a row vector: `nice` because it exercises expand_dims,
# broadcasting, 2-D layouts and 2-D addressing without any reduction.
import triton
import triton.language as tl


@triton.jit
def copy2d(in_ptr, out_ptr):
    rows = tl.arange(0, 8)[:, None]
    cols = tl.arange(0, 8)[None, :]
    offs = rows * 8 + cols
    x = tl.load(in_ptr + offs)
    tl.store(out_ptr + offs, x)
