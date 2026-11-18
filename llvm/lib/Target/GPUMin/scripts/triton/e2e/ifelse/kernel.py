"""Chapter 33 sample: a runtime if/else inside a kernel.

Each block owns a 32-element tile.  The branch condition is a scalar i1
computed from tl.program_id, so the choice is made at run time: block 0
takes the then branch and block 1 takes the else branch.  Nested scf.if
regions are the only new TTIR structure; values defined before the if stay
visible inside both branches.
"""

import triton
import triton.language as tl


@triton.jit
def ifelse_kernel(x_ptr, y_ptr, n):
    pid = tl.program_id(0)
    i = tl.arange(0, 32)
    v = tl.load(x_ptr + pid * 32 + i)
    if pid < n:
        tl.store(y_ptr + pid * 32 + i, v + 1.0)
    else:
        tl.store(y_ptr + pid * 32 + i, v * 2.0)
