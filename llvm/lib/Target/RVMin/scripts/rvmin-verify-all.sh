#!/usr/bin/env bash
#
# Chapter 15 gate: run every RVMin gate in one shot.
#
# Usage: rvmin-verify-all.sh <llvm-bin-dir>
#
# Five independent checks, in increasing order of cost:
#   1. the target is registered;
#   2. lit: CodeGen + MC tests;
#   3. encoding gate: byte-exact against the reference RISC-V assembler;
#   4. e2e: 10 C programs x 4 -O levels x 2 feature sets, round-tripped;
#   5. every CodeGen test at -O0..-O3 x {rv32i,+m} with -verify-machineinstrs.
#
# The QEMU run gate is separate (rvmin-run.sh) because it needs a linker and an
# emulator, which the other checks do not.
#
set -euo pipefail

BIN=${1:?usage: rvmin-verify-all.sh <llvm-bin-dir>}
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# scripts -> RVMin -> Target -> lib -> llvm -> repo root
W=$(cd "$HERE/../../../../.." && pwd)
TESTS="$W/llvm/test"

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

# 1. Registration.
"$BIN/llc" --version | grep -qw rvmin || fail "llc --version does not list rvmin"
echo "PASS 1/5: target registered"

# 2. lit.  Capture the log: lit prints one "did not find <tool>" note per
# missing binary, which is noise on a partial build.
LIT_LOG=$(mktemp)
if ! "$BIN/llvm-lit" -s "$TESTS/CodeGen/RVMin" "$TESTS/MC/RVMin" \
     > "$LIT_LOG" 2>&1; then
  grep -v 'note: Did not find' "$LIT_LOG" >&2
  fail "lit tests"
fi
grep -E 'Passed:|Total Discovered' "$LIT_LOG" || true
echo "PASS 2/5: lit (CodeGen + MC)"

# 3. Encoding gate.
"$HERE/check-rvmin-encodings.sh" "$BIN" || fail "encoding gate"
echo "PASS 3/5: encodings"

# 4. End-to-end corpus.
"$HERE/rvmin-e2e.sh" "$BIN" || fail "e2e corpus"
echo "PASS 4/5: e2e"

# 5. Machine-instruction verification at every optimization level.
CASES=0
for f in "$TESTS"/CodeGen/RVMin/*.ll; do
  for O in 0 1 2 3; do
    for MATTR in "" "+m"; do
      ARGS=(-march=rvmin -mtriple=riscv32-unknown-elf -O"$O" -verify-machineinstrs)
      [[ -n "$MATTR" ]] && ARGS+=(-mattr="$MATTR")
      "$BIN/llc" "${ARGS[@]}" "$f" -filetype=obj -o /dev/null \
        || fail "$(basename "$f") -O$O ${MATTR:-rv32i}"
      CASES=$((CASES + 1))
    done
  done
done
echo "PASS 5/5: $CASES llc runs with -verify-machineinstrs"

echo "ALL GATES PASS (run rvmin-run.sh separately for the QEMU check)"
