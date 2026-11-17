#!/usr/bin/env bash
#
# Chapter 50: run the Zve32x teaching subset under a user-mode emulator.
#
# Usage: rvmin-run-v.sh <llvm-bin-dir> [qemu]
#
# Exit status:
#   0  the program ran and printed "rvmin: ok"
#   2  the emulator does not implement the V extension (SIGILL); this is an
#      environment gap, not a compiler failure
#   1  a real compile/link/run failure
#
# This script is intentionally NOT part of rvmin-verify-all.sh: the default
# eight gates must not depend on a QEMU that implements V.  QEMU 6.2 and 7.2
# fault on vsetivli; QEMU 8.2 is the first version this course runs.
#
set -euo pipefail

BIN=${1:?usage: rvmin-run-v.sh <llvm-bin-dir> [qemu]}
QEMU=${2:-${QEMU:-qemu-riscv32-static}}
LD=${LD:-ld.lld}
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
SRC="$HERE/e2e-v/v-demo.c"
RT="$HERE/rt"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

CC="$BIN/clang"
LLC="$BIN/llc"
MC="$BIN/llvm-mc"
TRIPLE=riscv32-unknown-elf
MATTR=+f,+d,+v

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

command -v "$QEMU" >/dev/null 2>&1 || fail "emulator not found: $QEMU"
command -v "$LD" >/dev/null 2>&1 || fail "linker not found: $LD"

"$CC" --target="$TRIPLE" -march=rv32imafdv -mabi=ilp32d -O2 \
  -ffreestanding -fno-builtin -S -emit-llvm "$SRC" -o "$TMP/v-demo.ll" \
  || fail "clang could not compile v-demo.c"
"$LLC" -march=rvmin -mtriple="$TRIPLE" -O2 -mattr="$MATTR" \
  -verify-machineinstrs "$TMP/v-demo.ll" -o "$TMP/v-demo.s" \
  || fail "llc could not select v-demo.c"
"$MC" -triple=rvmin -mattr="$MATTR" -filetype=obj "$TMP/v-demo.s" \
  -o "$TMP/v-demo.o" || fail "llvm-mc rejected llc's output"

for f in rvmin-rt.S start.S; do
  "$MC" -triple=rvmin -mattr="$MATTR" -filetype=obj "$RT/$f" \
    -o "$TMP/${f%.S}.o" || fail "llvm-mc rejected $f"
done

"$LD" -m elf32lriscv -T "$RT/link.ld" -o "$TMP/v-demo" \
  "$TMP/start.o" "$TMP/v-demo.o" "$TMP/rvmin-rt.o" \
  || fail "ld.lld could not link v-demo"

set +e
OUT=$("$QEMU" "$TMP/v-demo" 2>&1)
STATUS=$?
set -e

if [[ "$STATUS" -eq 132 ]]; then
  echo "SKIP: $QEMU does not implement the V extension (SIGILL); use QEMU >= 8.2" >&2
  exit 2
fi

if [[ "$STATUS" -ne 0 ]]; then
  echo "$OUT" >&2
  fail "v-demo exited with $STATUS"
fi

echo "$OUT"
[[ "$OUT" == *"rvmin: ok"* ]] || fail "v-demo did not print 'rvmin: ok'"
