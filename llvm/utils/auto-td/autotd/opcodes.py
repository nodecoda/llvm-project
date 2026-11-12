"""Reader for riscv-opcodes style encoding databases.

A source file is one instruction per line::

    vadd.vv         31..26=0x00 vm vs2 vs1 14..12=0x0 vd 6..0=0x57

Tokens before the first ``=`` are *variable fields*; tokens containing ``=``
are *fixed bits*.  Field widths come from ``arg_lut.csv`` (``name,msb,lsb``).

The reader deliberately knows nothing about which repo a file belongs to; the
loader decides that by directory.
"""

from __future__ import annotations

import csv
import re
from pathlib import Path

from .model import BitRange, OpcodeRow

FIXED_RE = re.compile(r"^(?:(?P<msb>\d+)(?:\.\.(?P<lsb>\d+))?)=(?P<value>0x[0-9a-fA-F]+|\d+)$")


def key_for(mnemonic: str) -> str:
    """Database key for a mnemonic: dots and dashes become underscores."""
    return mnemonic.replace(".", "_").replace("-", "_")


def read_arg_lut(path: Path) -> dict[str, BitRange]:
    """Parse ``arg_lut.csv`` into field-name -> bit range."""
    ranges: dict[str, BitRange] = {}
    if not path.is_file():
        return ranges
    with path.open(newline="") as handle:
        for row in csv.reader(handle, skipinitialspace=True):
            if len(row) != 3:
                continue
            name, msb, lsb = row[0].strip(), row[1].strip(), row[2].strip()
            if not name.isidentifier():
                continue
            ranges[name] = BitRange(name, int(msb), int(lsb))
    return ranges


def read_source_file(path: Path, arg_lut: dict[str, BitRange]) -> dict[str, OpcodeRow]:
    """Parse one extension file into key -> row."""
    rows: dict[str, OpcodeRow] = {}
    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line or line.startswith("$"):
            continue
        tokens = line.split()
        mnemonic, rest = tokens[0], tokens[1:]
        fields = tuple(tok for tok in rest if "=" not in tok)
        fixed = tuple(tok for tok in rest if "=" in tok)
        ranges = {f: arg_lut[f] for f in fields if f in arg_lut}
        rows[key_for(mnemonic)] = OpcodeRow(mnemonic, fields, fixed, ranges, path)
    return rows


class OpcodeDatabase:
    """One or more opcode repos, keyed by ``(repo, extension, key)``."""

    def __init__(self) -> None:
        self._rows: dict[tuple[str, str, str], OpcodeRow] = {}
        self._luts: dict[str, dict[str, BitRange]] = {}

    def add_repo(self, name: str, root: Path, fallback_lut: dict[str, BitRange] | None = None) -> None:
        arg_lut = read_arg_lut(root / "arg_lut.csv") or (fallback_lut or {})
        self._luts[name] = arg_lut
        extensions = root / "extensions"
        if not extensions.is_dir():
            return
        for path in sorted(extensions.iterdir()):
            if not path.is_file() or path.name.startswith("."):
                continue
            for key, row in read_source_file(path, arg_lut).items():
                self._rows[(name, path.name, key)] = row

    def lookup(self, ref) -> OpcodeRow | None:
        return self._rows.get((ref.repo, ref.extension, ref.key))

    def lut_for(self, repo: str) -> dict[str, BitRange]:
        return self._luts.get(repo, {})

    def __len__(self) -> int:
        return len(self._rows)


def parse_fixed(token: str) -> tuple[int, int, int]:
    """``31..26=0x2a`` -> (31, 26, 42); ``25=0`` -> (25, 25, 0)."""
    match = FIXED_RE.match(token)
    if not match:
        raise ValueError(f"malformed fixed-bit token: {token!r}")
    msb = int(match.group("msb"))
    lsb = int(match.group("lsb") or msb)
    if msb < lsb:
        raise ValueError(f"fixed-bit token has msb < lsb: {token!r}")
    value = int(match.group("value"), 0)
    if value >= (1 << (msb - lsb + 1)):
        raise ValueError(f"fixed-bit value does not fit its range: {token!r}")
    return msb, lsb, value
