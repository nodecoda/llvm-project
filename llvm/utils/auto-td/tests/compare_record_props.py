#!/usr/bin/env python3
"""Compare the non-encoding properties of generated records.

``compare_riscv_records.py`` proves that the 32 ``Inst`` bits match.  It says
nothing about the properties that decide how a record is scheduled, allocated
and legalised: ``SchedRW``, ``Uses``/``Defs``, ``Constraints``, ``VMConstraint``,
``DestEEW``, ``VS1VS2Constraint`` and friends.  Those live in ``TSFlags`` or in
the instruction flags, and a wrong value there is a codegen bug that no
encoding test can see.

This tool snapshots those properties from a ``llvm-tblgen -print-records`` dump
before a migration and compares a later dump against it:

    compare_record_props.py --records /tmp/rvtrim.records \
        --opcodes llvm/lib/Target/RVTrim/auto-td/opcodes/extensions/rv_v \
        --write-golden llvm/lib/Target/RVTrim/auto-td/golden/v-all-props.txt

    compare_record_props.py --records /tmp/rvtrim-after.records \
        --golden llvm/lib/Target/RVTrim/auto-td/golden/v-all-props.txt

The golden file is JSON Lines: one record per line, keys sorted.  It is a
local migration aid, not an upstream artifact.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from autotd.model import record_name_for  # noqa: E402

DEF_RE = re.compile(r"^def (\w+) \{", re.M)

BIT_FIELDS = (
    "VMConstraint",
    "UseNamedOperandTable",
    "ReadsPastVL",
    "mayLoad",
    "mayStore",
    "hasSideEffects",
    "mayRaiseFPException",
    "isTiedPseudo",
    "HasSEWOp",
    "HasVLOp",
    "HasVecPolicyOp",
    "IsRVVWideningReduction",
    "UsesMaskPolicy",
    "IsSignExtendingOpW",
    "HasRoundModeOp",
    "UsesVXRM",
    "HasTWidenOp",
    "HasTmOp",
    "HasTkOp",
    "isCodeGenOnly",
    "isAsmParserOnly",
    "hasNoSchedulingInfo",
)

LIST_FIELDS = ("Uses", "Defs", "SchedRW", "Predicates")
STRING_FIELDS = ("Constraints", "AsmString")
DAG_FIELDS = ("OutOperandList", "InOperandList")
ENUM_FIELDS = {
    "VS1VS2Constraint": r"RVTrimVConstraint\s+VS1VS2Constraint\s*=\s*(\w+);",
    "DestEEW": r"EEW\s+DestEEW\s*=\s*(\w+);",
    "ElementsDependOn": r"EltDeps\s+ElementsDependOn\s*=\s*(\w+);",
    "Format": r"InstFormat\s+Format\s*=\s*(\w+);",
}


def _one_line(text: str) -> str:
    return re.sub(r"\s+", " ", text.strip())


def _items(text: str) -> list[str]:
    text = text.strip()
    return [] if not text else [item.strip() for item in text.split(",")]


def props_of(body: str) -> dict[str, object]:
    props: dict[str, object] = {}
    for name in BIT_FIELDS:
        match = re.search(rf"^  bit {name} = ([01]);$", body, re.M)
        if match:
            props[name] = int(match.group(1))
    for name in LIST_FIELDS:
        match = re.search(rf"^  list<\w+> {name} = \[(.*?)\];$", body, re.M)
        if match:
            props[name] = _items(match.group(1))
    for name in STRING_FIELDS:
        match = re.search(rf'^  string {name} = "(.*)";$', body, re.M)
        if match:
            props[name] = match.group(1)
    for name in DAG_FIELDS:
        match = re.search(rf"^  dag {name} = (.*);$", body, re.M)
        if match:
            props[name] = _one_line(match.group(1))
    for name, pattern in ENUM_FIELDS.items():
        match = re.search(rf"^  {pattern}$", body, re.M)
        if match:
            props[name] = match.group(1)
    match = re.search(r"^  bits<\d+> VLMul = \{(.*?)\};$", body, re.M)
    if match:
        props["VLMul"] = ",".join(_items(match.group(1)))
    return props


def parse_dump(text: str, names: set[str]) -> dict[str, dict]:
    records: dict[str, dict] = {}
    starts = list(DEF_RE.finditer(text))
    for index, match in enumerate(starts):
        name = match.group(1)
        if name not in names:
            continue
        end = starts[index + 1].start() if index + 1 < len(starts) else len(text)
        records[name] = props_of(text[match.start():end])
    return records


def names_from_opcodes(path: Path) -> list[str]:
    names: list[str] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        names.append(record_name_for(line.split()[0], ""))
    return names


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--records", required=True, type=Path, help="llvm-tblgen -print-records dump")
    parser.add_argument("--golden", required=True, type=Path, help="JSON Lines property snapshot")
    parser.add_argument("--opcodes", type=Path, help="opcode file used to derive record names (write mode)")
    parser.add_argument("--write-golden", action="store_true", help="write the snapshot instead of comparing")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args(argv)

    if args.write_golden:
        if args.opcodes is None:
            raise SystemExit("--write-golden requires --opcodes")
        names = names_from_opcodes(args.opcodes)
        records = parse_dump(args.records.read_text(encoding="utf-8", errors="replace"), set(names))
        missing = [name for name in names if name not in records]
        if missing:
            raise SystemExit(f"dump is missing {len(missing)} records, e.g. {missing[:5]}")
        args.golden.parent.mkdir(parents=True, exist_ok=True)
        with args.golden.open("w", encoding="utf-8") as stream:
            for name in sorted(records):
                stream.write(json.dumps({"name": name, **records[name]}, sort_keys=True) + "\n")
        print(f"wrote {len(records)} records to {args.golden}")
        return 0

    golden = {}
    for line in args.golden.read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        entry = json.loads(line)
        golden[entry.pop("name")] = entry

    records = parse_dump(args.records.read_text(encoding="utf-8", errors="replace"), set(golden))
    compared = missing = bad = 0
    for name in sorted(golden):
        want = golden[name]
        got = records.get(name)
        if got is None:
            missing += 1
            if args.verbose:
                print(f"MISSING  {name}")
            continue
        compared += 1
        diff = sorted(key for key in want if want[key] != got.get(key))
        if diff:
            bad += 1
            detail = ", ".join(f"{key}: want {want[key]!r} got {got.get(key)!r}" for key in diff[:4])
            print(f"MISMATCH {name}: {detail}")
    print(f"{compared} records compared, {bad} mismatching, {missing} missing")
    return 1 if bad or missing else 0


if __name__ == "__main__":
    sys.exit(main())
