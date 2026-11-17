#!/usr/bin/env bash
#
# Chapter 15 gate: run every RVMin gate in one shot.
#
# Usage: rvmin-verify-all.sh <llvm-bin-dir>
#
# Eight independent checks, in increasing order of cost:
#   1. the target is registered;
#   2. lit: CodeGen + MC tests;
#   3. encoding gate: byte-exact against the reference RISC-V assembler;
#   4. disassembler gate: asm -> obj -> disasm -> asm is byte-identical;
#   5. e2e: 10 C programs x 4 -O levels x 2 feature sets, round-tripped;
#   6. every CodeGen test at -O0..-O3 x {rv32i,+m} with -verify-machineinstrs
#      (tests that expect a diagnostic, i.e. "RUN: not llc", are skipped);
#   7. the TableGen DSL contracts for seven backends, the cross-backend
#      status matrix, the TSFlags field-effect report, and the upstream
#      BackEnds.md section (chapters 29-39);
#   8. the QEMU run gate (chapter 42): RV32I/M/A plus the F/D float and double
#      programs, which need ld.lld and qemu-riscv32-static.
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
echo "PASS 1/8: target registered"

# 2. lit.  Capture the log: lit prints one "did not find <tool>" note per
# missing binary, which is noise on a partial build.
LIT_LOG=$(mktemp)
if ! "$BIN/llvm-lit" -s "$TESTS/CodeGen/RVMin" "$TESTS/MC/RVMin" \
     > "$LIT_LOG" 2>&1; then
  grep -v 'note: Did not find' "$LIT_LOG" >&2
  fail "lit tests"
fi
grep -E 'Passed:|Total Discovered' "$LIT_LOG" || true
echo "PASS 2/8: lit (CodeGen + MC)"

# 3. Encoding gate.
"$HERE/check-rvmin-encodings.sh" "$BIN" || fail "encoding gate"
echo "PASS 3/8: encodings"

# 4. Disassembler round trip (chapter 20).
"$HERE/check-rvmin-disasm.sh" "$BIN" || fail "disassembler gate"
echo "PASS 4/8: disassembler"

# 5. End-to-end corpus.
"$HERE/rvmin-e2e.sh" "$BIN" || fail "e2e corpus"
echo "PASS 5/8: e2e"

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
echo "PASS 6/8: $CASES llc runs with -verify-machineinstrs"

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
python3 "$W/llvm/utils/TableGen/dsl-surface.py" --status-matrix --check \
  "$W/llvm/lib/Target/RVMin/docs/32-cross-backend-status.md" \
  --usage "$W/llvm/lib/Target/RVMin/RVMinRegisterInfo.td" \
          "$W/llvm/lib/Target/RVMin/RVMin.td" \
  || fail "cross-backend status matrix"
python3 "$W/llvm/utils/TableGen/dsl-surface.py" --effect TSFlags --check \
  "$W/llvm/lib/Target/RVMin/docs/33-field-effect.md" \
  || fail "field effect (TSFlags)"
python3 "$W/llvm/utils/TableGen/dsl-surface.py" --backend asm-matcher --check \
  "$W/llvm/lib/Target/RVMin/docs/34-asm-matcher-dsl.md" \
  --usage "$W/llvm/lib/Target/RVMin/RVMin.td" \
          "$W/llvm/lib/Target/RVMin/RVMinRegisterInfo.td" \
  || fail "TableGen DSL contract (asm-matcher)"
python3 "$W/llvm/utils/TableGen/dsl-surface.py" --backend dag-isel --check \
  "$W/llvm/lib/Target/RVMin/docs/36-dag-isel-dsl.md" \
  --usage "$W/llvm/lib/Target/RVMin/RVMinInstrInfo.td" \
          "$W/llvm/lib/Target/RVMin/RVMinRegisterInfo.td" \
  || fail "TableGen DSL contract (dag-isel)"
python3 "$W/llvm/utils/TableGen/dsl-surface.py" --backend fast-isel --check \
  "$W/llvm/lib/Target/RVMin/docs/37-fast-isel-dsl.md" \
  --usage "$W/llvm/lib/Target/RVMin/RVMinInstrInfo.td" \
          "$W/llvm/lib/Target/RVMin/RVMinRegisterInfo.td" \
  || fail "TableGen DSL contract (fast-isel)"
python3 "$W/llvm/utils/TableGen/dsl-surface.py" --backend disassembler --check \
  "$W/llvm/lib/Target/RVMin/docs/38-disassembler-dsl.md" \
  --usage "$W/llvm/lib/Target/RVMin/RVMinInstrInfo.td" \
          "$W/llvm/lib/Target/RVMin/RVMinInstrFormats.td" \
          "$W/llvm/lib/Target/RVMin/RVMinRegisterInfo.td" \
  || fail "TableGen DSL contract (disassembler)"
python3 "$W/llvm/utils/TableGen/dsl-surface.py" --summary --check \
  "$W/llvm/lib/Target/RVMin/docs/39-dsl-acceptance.md" \
  || fail "backend summary"
python3 - "$W" <<'PY' || fail "upstream BackEnds contract section"
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
doc = root / "llvm/docs/TableGen/BackEnds.md"
text = doc.read_text()
required = [
    "## Backend field contracts",
    "### The `(declaring class, field)` key",
    "### Four projection stages",
    "### Extracting a contract",
    "llvm/utils/TableGen/dsl-surface.py",
]
missing = [item for item in required if item not in text]
if not (root / "llvm/utils/TableGen/dsl-surface.py").is_file():
    missing.append("llvm/utils/TableGen/dsl-surface.py")
if missing:
    print("missing upstream docs content: " + ", ".join(missing),
          file=sys.stderr)
    sys.exit(1)
PY
echo "PASS 7/8: TableGen DSL contracts + status matrix + field effect"

# 8. QEMU run gate (chapters 12-19, 22 and 42).  This is the only gate that
# needs an external linker and emulator; both are checked here so the failure
# is a clear message rather than a confusing link error.
QEMU=${QEMU:-qemu-riscv32-static}
command -v "$QEMU" >/dev/null 2>&1 || fail "QEMU not found: $QEMU (set QEMU=...)"
command -v "${LD:-ld.lld}" >/dev/null 2>&1 || fail "linker not found: ${LD:-ld.lld}"
"$HERE/rvmin-run.sh" "$BIN" >/dev/null || fail "QEMU run gate"
echo "PASS 8/8: QEMU run (RV32I/M/A + F/D float and double programs)"

echo "ALL 8 GATES PASS"
