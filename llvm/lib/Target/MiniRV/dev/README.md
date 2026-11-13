# MiniRV developer checks

`LLVM_BUILD_DIR` selects the build tree (default
`/home/ubuntu/llvm/builds/dsl-tablegen`).

| script | what it proves |
|---|---|
| `minirv-smoke.sh` | auto-td unit tests and records, MC/CodeGen lit, e2e, code size |
| `e2e/run.sh` | MiniRV and the RISC-V reference return the same exit code for 17 programs |
| `bench/run.sh` | static instruction counts for five scalar kernels |

## End-to-end

Every `.ll` under `e2e/` defines `main` and returns a small exit code.  The
same IR is compiled twice:

```console
$ llc -mtriple=minirv64 -O2 -filetype=obj f.ll -o f.m.o
$ llc -mtriple=riscv64 -mattr=+m -O2 -filetype=obj f.ll -o f.r.o
```

`-mattr=+m` is required on the reference side because the generic `riscv64`
CPU does not enable the M extension by default, while MiniRV is RV64IM.  The
objects are linked against `e2e/start.s` (assembled with the reference target,
since the trimmed assembler has no `ecall` or register aliases) and executed
with `qemu-riscv64-static`; the exit codes must match.

The suite covers varargs, jump tables, indirect calls, recursion, dynamic
allocas, frame addresses, large frames, narrow stack arguments and i32 edge
cases.

## Static baseline

`bench/run.sh` compiles five scalar kernels for both targets and prints the
instruction counts.  The kernels are target-independent loops, so the ratio
shows how much of the reference's code the trimmed backend reproduces.  It is
a code-size baseline, not a performance claim.

## Requirements

`ld.lld` and `qemu-riscv64-static` must be on `PATH` (or set `LDLLD`/`QEMU`).
`e2e/run.sh` exits 77 when a tool is missing so a caller can skip it.
