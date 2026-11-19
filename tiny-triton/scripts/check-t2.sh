#!/usr/bin/env bash
#
# Tiny Triton chapter 2 gate: the frontend emits real `tinytt` IR.
#
# Usage: check-t2.sh <llvm-bin-dir>
#
set -uo pipefail

BIN=${1:?usage: check-t2.sh <llvm-bin-dir>}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
TMP=$(mktemp -d)
trap 'python3 -c "import shutil,sys; shutil.rmtree(sys.argv[1], ignore_errors=True)" "$TMP"' EXIT
export PYTHONDONTWRITEBYTECODE=1

FRONTEND="$BIN/tiny-triton"
OPT="$BIN/tiny-opt"
passed=0
failed=0

ok()  { passed=$((passed + 1)); echo "PASS $passed/4: $1"; }
bad() { failed=$((failed + 1)); echo "FAIL: $1"; }

# 1. The frontend runs and emits TTIR.
if "$FRONTEND" "$ROOT/kernels/vecadd.py" --emit ttir \
      --arg-type a_ptr=ptr --arg-type b_ptr=ptr --arg-type c_ptr=ptr \
      -o "$TMP/vecadd.ttir" 2>"$TMP/frontend.err"; then
  ok "tiny-triton --emit ttir runs"
else
  bad "frontend failed: $(cat "$TMP/frontend.err")"
fi

# 2. The emitted module is accepted by the dialect tool (round-trip).
if "$OPT" --verify-each "$TMP/vecadd.ttir" -o "$TMP/vecadd.canon" \
      2>"$TMP/opt.err"; then
  ok "emitted TTIR passes tiny-opt --verify-each"
else
  bad "tiny-opt rejected the frontend output: $(cat "$TMP/opt.err")"
fi

# 3. It matches the golden in canonical form.
if "$OPT" "$ROOT/kernels/vecadd.ttir" -o "$TMP/golden.canon" 2>/dev/null &&
   diff -u "$TMP/golden.canon" "$TMP/vecadd.canon" > "$TMP/diff.txt"; then
  ok "vecadd matches kernels/vecadd.ttir"
else
  bad "vecadd differs from the golden: $(head -20 "$TMP/diff.txt" 2>/dev/null)"
fi

# 4. Unsupported constructs are rejected with a source location.
if "$FRONTEND" "$ROOT/tests/frontend/loop_rejected.py" --emit ttir \
      --arg-type a_ptr=ptr -o /dev/null 2>"$TMP/loop.err"; then
  bad "control flow was accepted by a straight-line frontend"
else
  if grep -q 'only unrolls loops with constant bounds' "$TMP/loop.err" &&
     grep -q 'line 11' "$TMP/loop.err"; then
    ok "control flow is rejected with a location"
  else
    bad "wrong diagnostic: $(cat "$TMP/loop.err")"
  fi
fi

echo "----"
echo "chapter 2: $passed passed, $failed failed"
[ "$failed" -eq 0 ]
