"""Chapter 32 sample: single-head scaled dot-product attention on an 8x8 tile.

This is the third-course capstone: every piece comes from an earlier chapter
and the ISA gains no new instruction.

  * Q, K, V, P and O are 8x8 f32 tiles (4 warps x 8 lanes, 2 elements/thread);
  * S = Q K^T uses eight rank-1 updates from chapter 30;
  * the row-wise softmax is chapter 31's 2D reduction plus exp2;
  * O = P V re-reads P from a global scratch buffer, because the subset has no
    column slice of a computed tile.  The barrier between the store and the
    reload is the only new frontend surface: tl.debug_barrier().
"""

import triton
import triton.language as tl


@triton.jit
def attn_kernel(q_ptr, k_ptr, v_ptr, p_ptr, o_ptr):
    r = tl.arange(0, 8)[:, None]
    c = tl.arange(0, 8)[None, :]
    acc = tl.zeros((8, 8), tl.float32)
    for t in tl.static_range(0, 8):
        q = tl.load(q_ptr + r * 8 + t)
        kk = tl.load(k_ptr + c * 8 + t)
        acc = acc + q * kk
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
