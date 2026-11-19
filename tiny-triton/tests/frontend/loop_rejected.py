# Data-dependent control flow is still rejected.  Chapter 7 unrolls loops
# whose trip count is a compile-time constant, so the bound here is the block
# index: the frontend cannot know how many times to expand the body.
import triton
import triton.language as tl


@triton.jit
def accumulate(a_ptr):
    acc = 0
    for i in range(tl.program_id(0)):
        acc = acc + 1
    tl.store(a_ptr, acc)
