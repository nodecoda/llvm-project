"""Chapter 22 sample: scalar semantics with no tensor or memory operations.

The C++ frontend lowers this to the TTIR-min scalar subset:

    module {
      tt.func public @scalar_add(%n: i32) -> i32 {
        %0 = arith.constant 1 : i32
        %1 = arith.addi %n, %0 : i32
        tt.return %1 : i32
      }
    }
"""

import triton
import triton.language as tl


@triton.jit
def scalar_add(n):
    return n + 1


@triton.jit
def scalar_mix(a, b, c):
    tmp = (a + b) * 3
    shifted = tmp >> 2
    return shifted ^ c
