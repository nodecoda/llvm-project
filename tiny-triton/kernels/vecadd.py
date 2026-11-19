# Tiny Triton frontend input (chapter 2).  Kept next to the TTIR golden so the
# two can be compared: `tiny-triton kernels/vecadd.py --emit ttir ...`.
import triton
import triton.language as tl


@triton.jit
def vecadd(a_ptr, b_ptr, c_ptr):
    pid = tl.program_id(0)
    offs = pid * 1024 + tl.arange(0, 1024)
    a = tl.load(a_ptr + offs)
    b = tl.load(b_ptr + offs)
    tl.store(c_ptr + offs, a + b)
