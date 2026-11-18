#!/usr/bin/env python3
"""Regenerate expected.json for the chapter-36 causal attention program.

The model follows gpumin_sim.py exactly: every f32 add, sub, mul, div and
exp2 result is rounded back to single precision, and the row reductions run
left to right over the eight columns (emitReduce2D's order).  As a self-check
the same model replays chapter 32's unmasked attention program; only when it
reproduces all 64 values bit-exactly does it write the causal reference.

Usage: gen_expected.py [--write]
"""

import json
import math
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
LOG2E = None


def f32(x):
    return struct.unpack("<f", struct.pack("<f", x))[0]


LOG2E = f32(1.4426950408889634)
MASKED = f32(-1.0e9)


def exp2(a):
    try:
        return f32(math.pow(2.0, a))
    except OverflowError:
        return float("inf")


def load_memory(launch_path):
    launch = json.loads(launch_path.read_text())
    mem = {}
    for entry in launch["fill"]:
        assert entry["format"] == "f32"
        for i in range(entry["count"]):
            addr = entry["addr"] + 4 * i
            mem[addr] = f32(entry["start"] + i * entry["step"])
    return launch, mem


def tile(mem, base, rows=8, cols=8):
    return [[mem[base + 4 * (r * cols + c)] for c in range(8)]
            for r in range(rows)]


def compute(launch_path, causal):
    launch, mem = load_memory(launch_path)
    q = tile(mem, launch["args"][0])
    k = tile(mem, launch["args"][1])
    v = tile(mem, launch["args"][2])

    acc = [[0.0] * 8 for _ in range(8)]
    for t in range(8):
        for r in range(8):
            for c in range(8):
                acc[r][c] = f32(acc[r][c] + f32(q[r][t] * k[c][t]))
    if causal:
        for r in range(8):
            for c in range(8):
                acc[r][c] = f32(acc[r][c] + (0.0 if c <= r else MASKED))

    m = [max(acc[r]) for r in range(8)]
    p = [[exp2(f32(f32(acc[r][c] - m[r]) * LOG2E)) for c in range(8)]
         for r in range(8)]
    s = []
    for r in range(8):
        total = p[r][0]
        for c in range(1, 8):
            total = f32(total + p[r][c])
        s.append(total)
    p = [[f32(p[r][c] / s[r]) for c in range(8)] for r in range(8)]

    out = [[0.0] * 8 for _ in range(8)]
    for t in range(8):
        for r in range(8):
            for c in range(8):
                out[r][c] = f32(out[r][c] + f32(p[r][t] * v[t][c]))
    return [out[r][c] for r in range(8) for c in range(8)]


def main():
    write = "--write" in sys.argv[1:]
    attention = HERE.parent / "attention"
    causal = HERE

    want = json.loads((attention / "expected.json").read_text())["values"]
    got = compute(attention / "launch.json", causal=False)
    if got != want:
        print("FAIL: the model does not reproduce chapter 32's attention",
              file=sys.stderr)
        return 1

    values = compute(causal / "launch.json", causal=True)
    if write:
        doc = {
            "kernel": "causal_kernel",
            "arg_types": {
                "q_ptr": "ptr<f32>",
                "k_ptr": "ptr<f32>",
                "v_ptr": "ptr<f32>",
                "p_ptr": "ptr<f32>",
                "o_ptr": "ptr<f32>",
            },
            "format": "f32",
            "values": values,
        }
        (causal / "expected.json").write_text(json.dumps(doc, indent=2) + "\n")
        print(f"wrote {causal / 'expected.json'} ({len(values)} values)")
    else:
        print("model self-check passed; pass --write to update expected.json")
    return 0


if __name__ == "__main__":
    sys.exit(main())
