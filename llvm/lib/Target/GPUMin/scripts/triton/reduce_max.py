import triton
import triton.language as tl


@triton.jit
def reduce_max(x_ptr, out_ptr, BLOCK: tl.constexpr):
    pid = tl.program_id(0)
    offs = pid * BLOCK + tl.arange(0, BLOCK)
    x = tl.load(x_ptr + offs)
    largest = tl.max(x, axis=0)
    tl.store(out_ptr + pid, largest)
