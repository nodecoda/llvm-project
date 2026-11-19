# Tiny Triton frontend input (chapter 7b).  A 32x32x32 matmul on an 8x8 tile
# and a 2-D grid.  There is no tl.dot in this course: the inner dimension is
# unrolled with tl.static_range and every step is a rank-1 outer product,
#     C += A[:, k] * B[k, :],
# which is exactly what a dot instruction does, spelled out.  The pointer
# expression puts the tensor offset first (`b_ptr + cols + scalar`): a bare
# pointer plus a scalar is not a tensor, and the frontend rejects it.
import triton
import triton.language as tl


@triton.jit
def matmul2d(a_ptr, b_ptr, c_ptr):
    pid_m = tl.program_id(0)
    pid_n = tl.program_id(1)
    rows = tl.arange(0, 8)[:, None]
    cols = tl.arange(0, 8)[None, :]
    acc = tl.zeros((8, 8), tl.float32)
    for k in tl.static_range(0, 8):
        a = tl.load(a_ptr + (pid_m * 8 + rows) * 32 + k)
        b = tl.load(b_ptr + cols + (k * 32 + pid_n * 8))
        acc += a * b
    offs = (pid_m * 8 + rows) * 32 + pid_n * 8 + cols
    tl.store(c_ptr + offs, acc)
