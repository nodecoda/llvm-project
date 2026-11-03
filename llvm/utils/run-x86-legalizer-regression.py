#!/usr/bin/env python3
"""Run the X86 GlobalISel legalizer regression suite.

Tiers:
  smoke    build X86Tests, run the default suite, run GlobalISel lit (default)
  corpus   build, generate and check the full corpus, check the deep panel
  fuzz     build, run the fuzz tier selected by --fuzz-tier
  nightly  smoke + corpus + fuzz nightly
  all      smoke + corpus + fuzz at --fuzz-tier (default nightly)

The full corpus artifact is written outside the source tree and is never
committed.  The full and deep checks are disabled gtests, so they are only
reachable with --gtest_also_run_disabled_tests.

Examples:
  llvm/utils/run-x86-legalizer-regression.py --build /path/to/build
  llvm/utils/run-x86-legalizer-regression.py --build /path/to/build --tier corpus
  llvm/utils/run-x86-legalizer-regression.py --build /path/to/build --tier nightly
  llvm/utils/run-x86-legalizer-regression.py --build /path/to/build \
      --tier fuzz --fuzz-tier long
"""

import argparse
import hashlib
import os
import subprocess
import sys
import time
from pathlib import Path

TIERS = ("smoke", "corpus", "fuzz", "nightly", "all")
FUZZ_TIERS = ("smoke", "nightly", "weekly", "long")


def fail(message):
    print(f"error: {message}", file=sys.stderr)
    sys.exit(2)


def repo_root():
    return Path(__file__).resolve().parents[2]


def run(command, env=None, cwd=None):
    print("+ " + " ".join(str(part) for part in command), flush=True)
    started = time.monotonic()
    process = subprocess.run([str(part) for part in command], env=env, cwd=cwd)
    elapsed = time.monotonic() - started
    if process.returncode != 0:
        print(f"  failed after {elapsed:.1f}s", file=sys.stderr)
        sys.exit(process.returncode)
    print(f"  ok ({elapsed:.1f}s)", flush=True)


class Runner:
    def __init__(self, args):
        self.args = args
        self.root = repo_root()
        self.build_dir = Path(args.build).resolve()
        if not self.build_dir.is_dir():
            fail(f"build directory does not exist: {self.build_dir}")
        self.x86tests = self.build_dir / "unittests/Target/X86/X86Tests"
        if not self.x86tests.is_file():
            alternate = self.build_dir / "bin/X86Tests"
            if alternate.is_file():
                self.x86tests = alternate
            else:
                fail(
                    "X86Tests not found under "
                    f"{self.build_dir}/unittests/Target/X86 or {self.build_dir}/bin"
                )
        self.llvm_lit = self.build_dir / "bin/llvm-lit"
        self.full_artifact = Path(args.full_artifact).resolve()

    def build(self):
        if self.args.no_build:
            return
        run(["ninja", "-C", self.build_dir, "X86Tests"])

    def unit(self):
        run([self.x86tests])

    def disabled(self, test_filter, env=None):
        run(
            [
                self.x86tests,
                "--gtest_also_run_disabled_tests",
                f"--gtest_filter={test_filter}",
            ],
            env=env,
        )

    def lit(self):
        if not self.llvm_lit.is_file():
            fail(f"llvm-lit not found: {self.llvm_lit}")
        env = os.environ.copy()
        env["PATH"] = str(self.build_dir / "bin") + os.pathsep + env.get("PATH", "")
        run(
            [
                self.llvm_lit,
                "-s",
                "-j",
                str(self.args.jobs),
                self.root / "llvm/test/CodeGen/X86/GlobalISel",
            ],
            env=env,
        )

    def full_corpus(self):
        if not self.args.reuse_full:
            env = os.environ.copy()
            env["X86_LEGALIZER_FULL_OUT"] = str(self.full_artifact)
            self.disabled("*GenerateFullCorpus*", env=env)
        if not self.full_artifact.is_file():
            fail(f"full corpus missing: {self.full_artifact}")
        env = os.environ.copy()
        env["X86_LEGALIZER_FULL_IN"] = str(self.full_artifact)
        self.disabled("*FullCorpusDifferential*", env=env)
        size = self.full_artifact.stat().st_size
        print(f"full corpus artifact: {self.full_artifact} ({size} bytes)")

    def deep_panel(self):
        self.disabled("*DeepPanelDifferential*")

    def fuzz(self, tier):
        env = os.environ.copy()
        env["X86_LEGALIZER_FUZZ_TIER"] = tier
        self.disabled("*FuzzDifferentialTier*", env=env)

    def table_sha(self):
        table = self.build_dir / "lib/Target/X86/X86LegalizerTable.inc"
        if not table.is_file():
            fail(f"generated table not found: {table}")
        digest = hashlib.sha256(table.read_bytes()).hexdigest()
        print(f"X86LegalizerTable.inc sha256: {digest}")

    def smoke(self):
        self.build()
        self.unit()
        self.lit()

    def corpus(self):
        self.build()
        self.full_corpus()
        self.deep_panel()
        self.table_sha()

    def run(self):
        tier = self.args.tier
        if tier == "smoke":
            self.smoke()
        elif tier == "corpus":
            self.corpus()
        elif tier == "fuzz":
            self.build()
            self.fuzz(self.args.fuzz_tier)
        elif tier == "nightly":
            self.smoke()
            self.corpus()
            self.fuzz("nightly")
        elif tier == "all":
            self.smoke()
            self.corpus()
            self.fuzz(self.args.fuzz_tier)


def parse_args():
    parser = argparse.ArgumentParser(
        description="Run the X86 GlobalISel legalizer regression suite.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument(
        "--build",
        default=os.environ.get("LLVM_BUILD_DIR"),
        help="LLVM build directory (default: $LLVM_BUILD_DIR)",
    )
    parser.add_argument(
        "--tier",
        choices=TIERS,
        default="smoke",
        help="suite tier (default: smoke)",
    )
    parser.add_argument(
        "--fuzz-tier",
        choices=FUZZ_TIERS,
        default="nightly",
        help="fuzz tier for --tier fuzz/all (default: nightly)",
    )
    parser.add_argument(
        "--jobs", type=int, default=os.cpu_count() or 1, help="lit parallelism"
    )
    parser.add_argument(
        "--full-artifact",
        default=None,
        help="full corpus path (default: <build>/x86-legalizer-full.json)",
    )
    parser.add_argument(
        "--reuse-full",
        action="store_true",
        help="skip generation and check an existing full corpus artifact",
    )
    parser.add_argument(
        "--no-build", action="store_true", help="do not run ninja"
    )
    parser.add_argument(
        "--list", action="store_true", help="list tiers and exit"
    )
    args = parser.parse_args()
    if args.list:
        print("tiers: " + ", ".join(TIERS))
        print("fuzz tiers: " + ", ".join(FUZZ_TIERS))
        sys.exit(0)
    if not args.build:
        parser.error("--build or LLVM_BUILD_DIR is required")
    if args.full_artifact is None:
        args.full_artifact = str(
            Path(args.build).resolve() / "x86-legalizer-full.json"
        )
    return args


def main():
    args = parse_args()
    started = time.monotonic()
    Runner(args).run()
    print(f"tier {args.tier} passed in {time.monotonic() - started:.1f}s")


if __name__ == "__main__":
    main()
