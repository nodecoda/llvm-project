# Control flow is chapter 6.  Chapter 2 must reject it with a location.
import triton
import triton.language as tl


@triton.jit
def accumulate(a_ptr):
    acc = 0
    for i in range(4):
        acc = acc + tl.load(a_ptr + i)
    tl.store(a_ptr, acc)
