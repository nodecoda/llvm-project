#!/usr/bin/env bash
#
# Tiny Triton chapter 8 gate: the driver is a stage runner.  Every stage
# boundary is an MLIR module, `--input` resumes at a checkpoint, and asking
# for LLIR straight from TTIR is refused on purpose.
#
# Usage: check-t8.sh <llvm-bin-dir>
#
set -uo pipefail

BIN=${1:?usage: check-t8.sh <llvm-bin-dir>}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
TMP=$(mktemp -d)
trap 'python3 -c "import shutil,sys; shutil.rmtree(sys.argv[1], ignore_errors=True)" "$TMP"' EXIT
export PYTHONDONTWRITEBYTECODE=1

FRONTEND="$BIN/tiny-triton"
OPT="$BIN/tiny-opt"
passed=0
failed=0

ok()  { passed=$((passed + 1)); echo "PASS $passed/5: $1"; }
bad() { failed=$((failed + 1)); echo "FAIL: $1"; }

ARGS=(--arg-type a_ptr=ptr --arg-type b_ptr=ptr --arg-type c_ptr=ptr)

# 1. TTIR resume: the driver runs the same pass object `tiny-opt` registers,
#    so both outputs are the chapter 4 golden byte for byte.
if "$FRONTEND" --input ttir "$ROOT/kernels/vecadd.ttir" --emit ttgir \
      -o "$TMP/resume.ttgir" 2>"$TMP/resume.t8.err" &&
   "$OPT" --tiny-convert-tt-to-ttg "$ROOT/kernels/vecadd.ttir" \
      -o "$TMP/pass.ttgir" 2>"$TMP/pass.t4.err" &&
   cmp "$TMP/pass.ttgir" "$TMP/resume.ttgir" &&
   cmp "$ROOT/tests/passes/vecadd.ttgir" "$TMP/resume.ttgir"; then
  ok "--input ttir --emit ttgir is the tiny-opt pass and the T4 golden"
else
  bad "TTIR resume diverged: $(cat "$TMP"/resume.t8.err "$TMP"/pass.t4.err 2>/dev/null)"
fi

# 2. TTGIR resume: same argument one stage later.
if "$FRONTEND" --input ttgir "$TMP/resume.ttgir" --emit llir \
      -o "$TMP/resume.llir" 2>"$TMP/resume.t5.err" &&
   "$OPT" --tiny-convert-ttg-to-llvm "$TMP/resume.ttgir" \
      -o "$TMP/pass.llir" 2>"$TMP/pass.t5.err" &&
   cmp "$TMP/pass.llir" "$TMP/resume.llir" &&
   cmp "$ROOT/tests/passes/vecadd.llir" "$TMP/resume.llir"; then
  ok "--input ttgir --emit llir is the tiny-opt pass and the T5 golden"
else
  bad "TTGIR resume diverged: $(cat "$TMP"/resume.t5.err "$TMP"/pass.t5.err 2>/dev/null)"
fi

# 3. No skipped stage and no backwards motion.
refused=1
if "$FRONTEND" --input ttir "$ROOT/kernels/vecadd.ttir" --emit llir \
      -o "$TMP/skip.llir" >"$TMP/skip.out" 2>"$TMP/skip.err"; then
  bad "TTIR -> LLIR was accepted"
  refused=0
elif ! grep -q 'run --emit ttgir first' "$TMP/skip.err"; then
  bad "wrong diagnostic for TTIR -> LLIR: $(cat "$TMP/skip.err")"
  refused=0
fi
if "$FRONTEND" --input ttgir "$TMP/resume.ttgir" --emit ttir \
      -o "$TMP/back.ttir" >"$TMP/back.out" 2>"$TMP/back.err"; then
  bad "TTGIR -> TTIR was accepted"
  refused=0
elif ! grep -q 'cannot go back from ttgir to ttir' "$TMP/back.err"; then
  bad "wrong diagnostic for TTGIR -> TTIR: $(cat "$TMP/back.err")"
  refused=0
fi
[ "$refused" = 1 ] && ok "skipping a stage and going backwards are both refused"

# 4. A Python kernel enters at TTIR; all three entry points must agree with
#    the resume path, byte for byte.
if "$FRONTEND" "$ROOT/kernels/vecadd.py" --emit ttir "${ARGS[@]}" \
      -o "$TMP/py.ttir" 2>"$TMP/py.err" &&
   "$FRONTEND" "$ROOT/kernels/vecadd.py" --emit ttgir "${ARGS[@]}" \
      -o "$TMP/py.ttgir" 2>>"$TMP/py.err" &&
   "$FRONTEND" "$ROOT/kernels/vecadd.py" --emit llir "${ARGS[@]}" \
      -o "$TMP/py.llir" 2>>"$TMP/py.err" &&
   "$OPT" --verify-each "$TMP/py.ttir" -o /dev/null 2>>"$TMP/py.err" &&
   cmp "$TMP/py.ttgir" "$TMP/resume.ttgir" &&
   cmp "$TMP/py.llir" "$TMP/resume.llir" &&
   "$OPT" --verify-each "$TMP/py.ttgir" -o /dev/null 2>>"$TMP/py.err" &&
   "$OPT" --verify-each "$TMP/py.llir" -o /dev/null 2>>"$TMP/py.err"; then
  ok "kernel.py --emit ttir|ttgir|llir matches the checkpointed pipeline"
else
  bad "Python stage run diverged: $(cat "$TMP/py.err" 2>/dev/null)"
fi

# 5. The contract is documented and advertised: --help names both flags and
#    docs/08 carries the same three-stage table the gate checks.
if "$FRONTEND" --help >"$TMP/help.txt" 2>&1 &&
   grep -q -- '--input' "$TMP/help.txt" &&
   grep -q -- '--emit' "$TMP/help.txt" &&
   grep -q 'ttir' "$TMP/help.txt" &&
   grep -q 'ttgir' "$TMP/help.txt" &&
   grep -q 'llir' "$TMP/help.txt" &&
   grep -q -- '--input ttir' "$ROOT/docs/08-pipeline-and-resume.md" &&
   grep -q -- '--emit llir' "$ROOT/docs/08-pipeline-and-resume.md" &&
   grep -q 'verify-all.sh' "$ROOT/docs/08-pipeline-and-resume.md"; then
  ok "--help and docs/08 describe the same stage contract"
else
  bad "CLI and docs/08 disagree: $(head -20 "$TMP/help.txt" 2>/dev/null)"
fi

echo "----"
echo "chapter 8: $passed passed, $failed failed"
[ "$failed" -eq 0 ]
