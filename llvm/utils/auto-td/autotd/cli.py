"""Command line entry point for auto-td.

The flag names are compatible with the reference tool so existing build
recipes keep working; everything behind them is a fresh implementation.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from . import coverage, emit
from .check import coverage_problems, reference_problems, unknown_keys, validate
from .loader import load_config, load_instructions
from .opcodes import OpcodeDatabase

DEFAULT_OUTPUTS = {
    "instrinfo": "AutoTdInstrInfo.inc",
    "manifests": "AutoTdManifests.inc",
    "coverage": "AutoTdCoverage.md",
}


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="auto-td-gen",
        description="Generate TableGen instruction records from an opcode database.",
    )
    parser.add_argument("--target-root", "--ysx-root", dest="target_root", required=True,
                        help="target directory containing auto-td/")
    parser.add_argument("--riscv-opcodes", required=True, help="upstream opcode database root")
    parser.add_argument("--ysx-opcodes", help="local opcode database root (optional)")
    parser.add_argument("--out-dir", required=True)
    parser.add_argument("--coverage", help="coverage report path")
    parser.add_argument("--clang-builtins-td", help="emit Clang builtin .td")
    parser.add_argument("--clang-builtin-cg-inc", help="emit CGBuiltin dispatch .inc")
    parser.add_argument("--llvm-intrinsics-td", help="emit LLVM intrinsic .td")
    parser.add_argument("--record-prefix", help="override config record_prefix")
    parser.add_argument("--check-only", action="store_true", help="validate and exit")
    parser.add_argument("--strict", action="store_true", help="treat unused YAML keys as errors")
    parser.add_argument("--quiet", action="store_true")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    target_root = Path(args.target_root)
    config = load_config(target_root)

    db = OpcodeDatabase()
    db.add_repo("riscv-opcodes", Path(args.riscv_opcodes))
    if args.ysx_opcodes:
        db.add_repo("ysx-opcodes", Path(args.ysx_opcodes), db.lut_for("riscv-opcodes"))

    specs = load_instructions(target_root, db, config)
    prefix = args.record_prefix or str(config.get("record_prefix", "AUTO"))
    # ``emit`` reads the prefix from the config; keep one source of truth so
    # ``--record-prefix`` really namespaces the generated records.
    config = dict(config, record_prefix=prefix)

    for warning in unknown_keys(specs):
        print(f"auto-td: warning: {warning}", file=sys.stderr)

    problems = validate(specs, prefix, strict=args.strict)
    problems += coverage_problems(db, config, specs)
    problems += reference_problems(db, config, specs, target_root)
    if problems:
        for problem in problems:
            print(f"auto-td: error: {problem}", file=sys.stderr)
        return 1
    if args.check_only:
        if not args.quiet:
            print(f"auto-td: {len(specs)} instructions OK")
        return 0

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / DEFAULT_OUTPUTS["instrinfo"]).write_text(
        emit.render_instrinfo(specs, config), encoding="utf-8"
    )
    (out_dir / DEFAULT_OUTPUTS["manifests"]).write_text(
        emit.render_manifests(specs), encoding="utf-8"
    )
    coverage_path = Path(args.coverage) if args.coverage else out_dir / DEFAULT_OUTPUTS["coverage"]
    coverage_path.parent.mkdir(parents=True, exist_ok=True)
    coverage_path.write_text(coverage.render(specs), encoding="utf-8")

    if args.clang_builtins_td:
        path = Path(args.clang_builtins_td)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(emit.render_clang_builtins_td(specs, config), encoding="utf-8")
    if args.clang_builtin_cg_inc:
        path = Path(args.clang_builtin_cg_inc)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(emit.render_clang_builtin_cg_inc(specs, config), encoding="utf-8")
    if args.llvm_intrinsics_td:
        path = Path(args.llvm_intrinsics_td)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(emit.render_llvm_intrinsics_td(specs, config), encoding="utf-8")

    if not args.quiet:
        print(f"auto-td: wrote {len(specs)} instruction records to {out_dir}")
    return 0
