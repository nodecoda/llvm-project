# GPU-simulated GM32 interpreter (feasibility PoC)

This directory answers one question with measurements: **can the local NVIDIA
GPU be used to *simulate* GPUMin programs?**

Short answer: yes, in the "one CUDA thread interprets one GM32 lane" sense.
The GPU cannot execute GM32 machine code natively, but it can host a GM32
interpreter with a clean block/warp/lane mapping.

## Mapping

| GM32 concept | CUDA concept |
| --- | --- |
| one GM32 block | one CUDA block |
| 1 GM32 block = 4 warps x 8 lanes = 32 lanes | `blockDim.x = 32` |
| one GM32 lane | one CUDA thread |
| GM32 register file (per lane) | thread-local `int r[32]` |
| GM32 predicate file | thread-local `int p[8]` |
| GM32 shared memory | `extern __shared__ int sh[]` |
| GM32 `SYNC` (block barrier) | `__syncthreads()` |
| GM32 global memory | a flat device buffer addressed by byte offset |

Note that 4 GM32 warps share one hardware warp of 32 threads; this is exactly
the "teaching warp = 8 lanes" packing described in the plan.

## Programs used

Both programs are emitted by `mkprog.py` into `prog.h` and are also executed by
the Python reference interpreter in the same file:

- `vecadd`: `out[gid] = a[gid] + b[gid]`, predicated on `gid < n`.
  Exercises: special registers, address arithmetic, predication, global
  load/store.
- `reduce`: block-level tree reduction of 32 values through shared memory.
  Exercises: shared load/store and 6 block barriers.

## Measured on the T4 (2026-11-18)

```
GPU: Tesla T4, sm_75, warpSize=32, sharedMemPerBlock=49152
CUDA 12.4, driver 535.216.01

vecadd  interp mismatches=0
vecadd  cuda_ref mismatches=0
reduce  interp mismatches=0 (of 32768 blocks)

path                    ms/launch      Gelem/s       Ginstr/s
GM32 interp on GPU         1.1954         0.88          17.54
native CUDA vecadd         0.0745        14.07              -
GM32 interp reduce         1.3784            -              -
native CUDA reduce         0.0943            -              -

interp/native overhead vecadd: 16x

CPU reference interpreter (CPython 3): 3.38 M lane-instructions/s
=> GPU interpretation is ~5200x faster than the CPython interpreter.
```

Interpretation: a GPU-hosted GM32 interpreter is roughly four orders of
magnitude faster than the CPython simulator, which makes it usable for
medium-sized e2e runs; it is still ~16x slower than native CUDA, which is the
expected price of fetch/decode/execute in device code.

## Hard constraints (why this is not a drop-in replacement for the CPU simulator)

1. **Barriers must be uniform.** `SYNC` maps to `__syncthreads()`, which
   requires every thread of the block to reach it. A GM32 program that lets
   some lanes `EXIT` before a barrier will deadlock the CUDA kernel.
2. **Divergence is handled twice.** GM32 predication is emulated directly, but
   the CUDA warp also serializes divergent paths inside its 32 threads. The
   result stays correct, but it is not a faithful model of GM32 warp
   scheduling; that is what the chapter 14 SIMT stack is for.
3. **Per-lane state is device local memory.** `int r[32]` plus a fetch loop
   costs ~16x versus native code, and register pressure limits occupancy.
4. **This proves execution, not encoding.** The PoC decodes a `struct Inst`
   built by a Python helper, not real 32-bit GM32 words, so it does not
   validate `GPUMinMCCodeEmitter` or the ELF writer.

## How to run

```sh
cd llvm/lib/Target/GPUMin/tools/gpu-sim-poc
PYTHONDONTWRITEBYTECODE=1 python3 mkprog.py emit > prog.h
nvcc -O2 -arch=sm_75 sim.cu -o gpumin-gpu-sim
./gpumin-gpu-sim
# CPU reference interpreter throughput (slower, ~6 s for 1 Mi elements):
PYTHONDONTWRITEBYTECODE=1 python3 mkprog.py bench 1048576
```

## Where this lands in the tutorial

Appendix A.4 of the plan: an optional "GPU-accelerated simulator" appendix,
after chapters 00-20. The CPU Python simulator (chapter 12) remains the
correctness oracle; the GPU interpreter is a throughput option and a
real-hardware teaching aid.
