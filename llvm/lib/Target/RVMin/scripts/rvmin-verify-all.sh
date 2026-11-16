#!/usr/bin/env bash
#
# Chapter 15 gate: run every RVMin gate in one shot.
#
# Usage: rvmin-verify-all.sh <llvm-bin-dir>
#
# Seven independent checks, in increasing order of cost:
#   1. the target is registered;
#   2. lit: CodeGen + MC tests;
#   3. encoding gate: byte-exact against the reference RISC-V assembler;
#   4. disassembler gate: asm -> obj -> disasm -> asm is byte-identical;
#   5. e2e: 10 C programs x 4 -O levels x 2 feature sets, round-tripped;
#   6. every CodeGen test at -O0..-O3 x {rv32i,+m} with -verify-machineinstrs
#      (tests that expect a diagnostic, i.e. "RUN: not llc", are skipped);
#   7. the -gen-register-info DSL contract (chapter 29).
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
echo "PASS 1/7: target registered"

# 2. lit.  Capture the log: lit prints one "did not find <tool>" note per
# missing binary, which is noise on a partial build.
LIT_LOG=$(mktemp)
if ! "$BIN/llvm-lit" -s "$TESTS/CodeGen/RVMin" "$TESTS/MC/RVMin" \
     > "$LIT_LOG" 2>&1; then
  grep -v 'note: Did not find' "$LIT_LOG" >&2
  fail "lit tests"
fi
grep -E 'Passed:|Total Discovered' "$LIT_LOG" || true
echo "PASS 2/7: lit (CodeGen + MC)"

# 3. Encoding gate.
"$HERE/check-rvmin-encodings.sh" "$BIN" || fail "encoding gate"
echo "PASS 3/7: encodings"

# 4. Disassembler round trip (chapter 20).
"$HERE/check-rvmin-disasm.sh" "$BIN" || fail "disassembler gate"
echo "PASS 4/7: disassembler"

# 5. End-to-end corpus.
"$HERE/rvmin-e2e.sh" "$BIN" || fail "e2e corpus"
echo "PASS 5/7: e2e"

# 6. Machine-instruction verification at every optimization level.
CASES=0
for f in "$TESTS"/CodeGen/RVMin/*.ll; do
  # Expected-failure tests are covered by lit; here every input must compile.
  grep -q 'RUN: not llc' "$f" && continue
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
echo "PASS 6/7: $CASES llc runs with -verify-machineinstrs"

# 7. The TableGen DSL contracts must stay in sync with the emitter sources and
# with RVMin's own .td usage (chapters 29-30).
python3 "$W/llvm/utils/TableGen/dsl-surface.py" --check \
  "$W/llvm/lib/Target/RVMin/docs/29-register-info-dsl.md" \
  --usage "$W/llvm/lib/Target/RVMin/RVMinRegisterInfo.td" \
          "$W/llvm/lib/Target/RVMin/RVMinCallingConv.td" \
  || fail "TableGen DSL contract (register-info)"
python3 "$W/llvm/utils/TableGen/dsl-surface.py" --backend instr-info --check \
  "$W/llvm/lib/Target/RVMin/docs/30-instr-info-dsl.md" \
  --usage "$W/llvm/lib/Target/RVMin/RVMin.td" \
  || fail "TableGen DSL contract (instr-info)"
python3 "$W/llvm/utils/TableGen/dsl-surface.py" --backend asm-writer --check \
  "$W/llvm/lib/Target/RVMin/docs/31-asm-writer-dsl.md" \
  --usage "$W/llvm/lib/Target/RVMin/RVMinRegisterInfo.td" \
  || fail "TableGen DSL contract (asm-writer)"
echo "PASS 7/7: TableGen DSL contracts (register-info + instr-info + asm-writer)"

echo "ALL GATES PASS (run rvmin-run.sh separately for the QEMU check)"
