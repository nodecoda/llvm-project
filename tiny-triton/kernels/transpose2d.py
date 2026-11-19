# Tiny Triton frontend input (chapter 7b).  An 8x8 transpose: every element has
# to move to the thread that owns the mirrored coordinates, which is the first
# kernel in the course that needs shared memory for a layout change.
import triton
import triton.language as tl


@triton.jit
def transpose2d(in_ptr, out_ptr):
    rows = tl.arange(0, 8)[:, None]
    cols = tl.arange(0, 8)[None, :]
    offs = rows * 8 + cols
    x = tl.load(in_ptr + offs)
    y = tl.trans(x)
    tl.store(out_ptr + offs, y)
