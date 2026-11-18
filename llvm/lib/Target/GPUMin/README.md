# GPUMin

A minimal SIMT (GPU) backend for LLVM, written as a step-by-step tutorial.  The
goal is legibility and verifiability, not speed: every layer -- instruction
encoding, register allocation, warp divergence, shared memory, barriers, the
issue model -- has a chapter and a gate.

## What it is

- A real LLVM target: `llc -march=gpumin`, `llvm-mc -triple=gpumin`.
- A SIMT execution model: 8 lanes per warp, 4 warps per block, structured
  divergence with a frame stack, `bar.sync`, shared memory in `addrspace(2)`.
- A teaching timing model: ALU/LSU/BAR issue slots, load latency, occupancy.

## What it is not

- Not a complete GPU: no texture units, no warp shuffles, no coalescing model,
  no multi-block residency scheduler.
- Not cycle-accurate: the chapter 18/19 numbers come from a fixed-slot,
  fixed-latency model, not from a real pipeline.
- Not a performance tool: `--stats` is for comparing the *shape* of two
  implementations, not for extrapolating absolute numbers.

## Layout

| Path | Contents |
| --- | --- |
| `GPUMinInstrFormats.td` | encoding formats and the opcode map |
| `GPUMinInstrInfo.td` | instruction definitions and selection patterns |
| `GPUMinISelDAGToDAG.cpp` | constants, address-space-aware memory selection |
| `GPUMinISelLowering.cpp` | calling convention, legalization, lowering |
| `GPUMinAsmPrinter.cpp` | MachineInstr -> MCInst, shared-global elision |
| `scripts/simt-model.json` | the single source of truth for model numbers |
| `tools/gpumin_sim.py` | ELF loader, executor, block scheduler, statistics |
| `tools/gpumin_model.py` | model access + self-test |
| `docs/` | the tutorial, one chapter per file |
| `demos/` | seven runnable kernels plus `run-demos.sh` and a walkthrough |

## Build and verify

```sh
ninja llc llvm-mc llvm-objdump
scripts/gpumin-verify-all.sh <llvm-bin-dir>     # 8 gates
scripts/check-ch20.sh <llvm-bin-dir>            # one chapter
demos/run-demos.sh <llvm-bin-dir>               # seven feature demos
```

## Start reading

`docs/README.md` has the chapter index and the quick-start commands.
