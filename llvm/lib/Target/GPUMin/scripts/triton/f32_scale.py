import triton
import triton.language as tl


@triton.jit
def f32_scale(x_ptr, out_ptr, BLOCK: tl.constexpr):
    offs = tl.arange(0, BLOCK)
    x = tl.load(x_ptr + offs)
    y = x * 1.5
    keep = x < y
    tl.store(out_ptr + offs, y, mask=keep)
