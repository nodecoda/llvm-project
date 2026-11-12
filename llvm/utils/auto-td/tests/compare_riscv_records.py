#!/usr/bin/env python3
"""Compare auto-td records against a backend's own TableGen records.

The point of auto-td is that a generated record is *the same record* the
backend would have written by hand.  This script proves it for a whole
instruction set: it reads the generated ``AutoTdInstrInfo.inc``, finds the
same-named ``def`` in a ``llvm-tblgen -print-records`` dump, and compares the
32 ``Inst`` bits.

Variable identifiers are record-local: they only have to match the name of an
operand, so ``bits<5> rs1`` and ``bits<5> zimm5`` in the same bit slot are the
same encoding.  Each record is therefore canonicalised to "constant" or "the
exact set of (bit, index) pairs this variable covers"; a wrong bit position, a
wrong slice, or a wrong constant still mismatches.

Produce the reference dump with the backend's own .td files, e.g.::

    llvm-tblgen -print-records -I llvm/lib/Target/RISCV \
        llvm/lib/Target/RISCV/RISCV.td > /tmp/riscv.records

then::

    compare_riscv_records.py --inc <build>/AutoTdInstrInfo.inc \
        --records /tmp/riscv.records [--strip-prefix RVTRIM_AUTO]
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

DEF_RE = re.compile(r"^def (\w+) \{", re.M)
INST_RE = re.compile(r"field bits<(\d+)> Inst = \{(.*?)\};", re.S)
BIT_RE = re.compile(r"([A-Za-z_]\w*)(?:\{(\d+)\})?")
ASSIGN_RE = re.compile(r"let Inst\{(\d+)(?:-(\d+))?\} = ([^;]+);")


def expand(expr: str, msb: int, lsb: int, where: str) -> dict[int, tuple]:
    """Expand one ``let Inst{...} = expr;`` into per-bit values."""
    width = msb - lsb + 1
    literal = re.fullmatch(r"0b([01]+)", expr)
    if literal:
        bits = literal.group(1)
        if len(bits) != width:
            raise SystemExit(f"{where}: literal {expr} is not {width} bits")
        return {msb - i: ("c", ch) for i, ch in enumerate(bits)}
    sliced = re.fullmatch(r"([A-Za-z_]\w*)(?:\{(\d+)(?:-(\d+))?\})?", expr)
    if not sliced:
        raise SystemExit(f"{where}: cannot parse bit expression {expr!r}")
    name, hi, lo = sliced.group(1), sliced.group(2), sliced.group(3)
    if hi is None:
        hi, lo = width - 1, 0
    hi = int(hi)
    lo = int(lo) if lo is not None else hi
    if hi - lo + 1 != width:
        raise SystemExit(f"{where}: slice {expr} is not {width} bits")
    return {msb - i: ("v", name, hi - i) for i in range(width)}


def canonical(bits: dict[int, tuple]) -> dict[int, tuple]:
    groups: dict[str, set[tuple[int, int]]] = {}
    for bit, value in bits.items():
        if value[0] == "v":
            groups.setdefault(value[1], set()).add((bit, value[2]))
    return {
        bit: ("c", value[1]) if value[0] == "c" else ("v", frozenset(groups[value[1]]))
        for bit, value in bits.items()
    }


def parse_generated(text: str) -> dict[str, dict[int, tuple]]:
    records: dict[str, dict[int, tuple]] = {}
    for match in re.finditer(r"^def (\w+) : .*\{$", text, re.M):
        body = text[match.end():text.index("\n}", match.end())]
        bits: dict[int, tuple] = {}
        for msb, lsb, expr in ASSIGN_RE.findall(body):
            msb = int(msb)
            lsb = int(lsb) if lsb else msb
            bits.update(expand(expr.strip(), msb, lsb, match.group(1)))
        records[match.group(1)] = canonical(bits)
    return records


def parse_reference(text: str) -> dict[str, dict[int, tuple]]:
    starts = [(m.group(1), m.start()) for m in DEF_RE.finditer(text)]
    records: dict[str, dict[int, tuple]] = {}
    for name, start in starts:
        end = text.find("\ndef ", start + 1)
        body = text[start:end if end >= 0 else len(text)]
        match = INST_RE.search(body)
        if not match:
            continue
        # ``-print-records`` prints the Inst field MSB first.
        values = [item.strip() for item in match.group(2).split(",")]
        bits: dict[int, tuple] = {}
        for bit, value in zip(range(len(values) - 1, -1, -1), values):
            if value in ("0", "1"):
                bits[bit] = ("c", value)
            else:
                parsed = BIT_RE.fullmatch(value)
                if not parsed:
                    raise SystemExit(f"{name}: cannot parse reference bit {value!r}")
                # A bare identifier is a one-bit variable (e.g. ``vm``).
                bits[bit] = ("v", parsed.group(1),
                             int(parsed.group(2)) if parsed.group(2) else 0)
        records[name] = canonical(bits)
    return records


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--inc", required=True, type=Path, help="generated AutoTdInstrInfo.inc")
    parser.add_argument("--records", required=True, type=Path, help="llvm-tblgen -print-records dump")
    parser.add_argument("--strip-prefix", default="", help="record prefix to strip before lookup")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args(argv)

    generated = parse_generated(args.inc.read_text(encoding="utf-8"))
    reference = parse_reference(args.records.read_text(encoding="utf-8", errors="replace"))
    prefix = f"{args.strip_prefix}_" if args.strip_prefix else ""

    compared = missing = bad = 0
    for name, got in sorted(generated.items()):
        if prefix and name.startswith(prefix):
            lookup = name[len(prefix):]
        elif prefix:
            continue
        else:
            lookup = name
        want = reference.get(lookup)
        if want is None:
            missing += 1
            if args.verbose:
                print(f"MISSING  {lookup} (not in reference dump)")
            continue
        compared += 1
        diff = [(b, want.get(b), got.get(b)) for b in range(31, -1, -1) if want.get(b) != got.get(b)]
        if diff:
            bad += 1
            print(f"MISMATCH {lookup}: {diff[:6]}")
    print(f"{compared} instructions compared, {bad} mismatching, {missing} not in reference")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
