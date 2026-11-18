#!/usr/bin/env python3
"""Run one chapter-27 e2e program.

Usage: run_e2e.py <program-dir> --bin <llvm-bin-dir> --sim <gpumin_sim.py>

The program directory holds kernel.py, launch.json and expected.json.  The
script compiles the kernel to GPUMin LLVM IR and an object, runs it on the
simulator, and compares the dumped words with the reference values.  f32
dumps are compared bit-exactly after converting the simulator's integer
words back to floats (the ISA stores raw 32-bit patterns).
"""

import argparse
import json
import struct
import subprocess
import sys
import tempfile
from pathlib import Path


def fail(msg):
    print(f"FAIL: {msg}", file=sys.stderr)
    return 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--bin", required=True)
    ap.add_argument("--sim", required=True)
    args = ap.parse_args()

    pdir = Path(args.dir)
    expected = json.loads((pdir / "expected.json").read_text())
    kernel = expected["kernel"]
    fmt = expected.get("format", "i32")
    values = expected["values"]

    tool = Path(args.bin) / "gpumin-triton"
    llc = Path(args.bin) / "llc"
    if not tool.is_file():
        return fail(f"missing {tool}")
    if not llc.is_file():
        return fail(f"missing {llc}")

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        emit = [str(tool), "--emit-llvm"]
        for name, value in expected.get("constexpr", {}).items():
            emit += ["--constexpr", f"{name}={value}"]
        for name, ty in expected.get("arg_types", {}).items():
            emit += ["--arg-type", f"{name}={ty}"]
        emit += [str(pdir / "kernel.py"), "-o", str(tmp / "kernel.ll")]
        r = subprocess.run(emit, capture_output=True, text=True)
        if r.returncode:
            return fail(f"gpumin-triton failed:\n{r.stderr}")

        r = subprocess.run(
            [str(llc), "-march=gpumin", "-O1", "-verify-machineinstrs",
             "-filetype=obj", str(tmp / "kernel.ll"), "-o", str(tmp / "kernel.o")],
            capture_output=True, text=True)
        if r.returncode:
            return fail(f"llc failed:\n{r.stderr}")

        r = subprocess.run(
            [sys.executable, args.sim, str(tmp / "kernel.o"),
             "--kernel", kernel, "--launch", str(pdir / "launch.json")],
            capture_output=True, text=True)
        if r.returncode:
            return fail(f"simulator failed:\n{r.stderr}")

        words = [int(w) for w in r.stdout.split()]
        if fmt == "f32":
            got = [struct.unpack("<f", struct.pack("<I", w & 0xffffffff))[0]
                   for w in words]
        else:
            got = words
        if got != values:
            return fail(f"{pdir.name}: got {got!r}, want {values!r}")

    print(f"PASS: {pdir.name} ({fmt}, {len(values)} values)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
