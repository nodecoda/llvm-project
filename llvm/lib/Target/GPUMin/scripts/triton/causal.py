"""Chapter 36 sample: causal attention with a masked bias load.

The kernel is chapter 32's attention with one extra tile: an additive causal
mask.  ``bias`` is loaded with ``mask=c <= r`` and ``other=-1.0e9``:

  * allowed lanes (c <= r) read ``p[0]``, which the launch descriptor zero-fills;
  * future lanes (c > r) get ``other``, a large negative finite value.

``exp2`` underflows ``-1e9 * log2(e)`` to exactly zero, so the softmax over a
row only counts the keys at or before the query.  No inf or NaN arithmetic is
needed, which keeps the simulator's f32 reference exact.
"""

import triton
import triton.language as tl


@triton.jit
def causal_kernel(q_ptr, k_ptr, v_ptr, p_ptr, o_ptr):
    r = tl.arange(0, 8)[:, None]
    c = tl.arange(0, 8)[None, :]
    acc = tl.zeros((8, 8), tl.float32)
    for t in tl.static_range(0, 8):
        q = tl.load(q_ptr + r * 8 + t)
        kk = tl.load(k_ptr + c * 8 + t)
        acc = acc + q * kk
    bias = tl.load(p_ptr + r * 0 + c * 0, mask=c <= r, other=-1.0e9)
    acc = acc + bias
    m = tl.max(acc, axis=1, keep_dims=True)
    p = tl.exp2((acc - m) * 1.4426950408889634)
    s = tl.sum(p, axis=1, keep_dims=True)
    p = p / s
    tl.store(p_ptr + r * 8 + c, p)
    tl.debug_barrier()
    out = tl.zeros((8, 8), tl.float32)
    for t in tl.static_range(0, 8):
        pc = tl.load(p_ptr + r * 8 + t)
        vv = tl.load(v_ptr + t * 8 + c)
        out = out + pc * vv
    tl.store(o_ptr + r * 8 + c, out)
