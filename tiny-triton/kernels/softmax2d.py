# Tiny Triton frontend input (chapter 7b).  A row-wise, base-2 softmax over an
# 8x8 tile: max, exp2, sum and divide, with keep_dims so the row vector keeps
# the shape the broadcast needs.
#
# GPUMin exposes exp2, not exp, so this is "softmax in base 2":
#     y[r, c] = 2 ** (x[r, c] - max_r) / sum_c 2 ** (x[r, c] - max_r)
import triton
import triton.language as tl


@triton.jit
def softmax2d(in_ptr, out_ptr):
    rows = tl.arange(0, 8)[:, None]
    cols = tl.arange(0, 8)[None, :]
    offs = rows * 8 + cols
    x = tl.load(in_ptr + offs)
    m = tl.max(x, axis=1, keep_dims=True)
    e = tl.exp2(x - m)
    s = tl.sum(e, axis=1, keep_dims=True)
    y = e / s
    tl.store(out_ptr + offs, y)
