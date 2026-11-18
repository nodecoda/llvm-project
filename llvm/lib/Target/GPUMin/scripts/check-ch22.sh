#!/usr/bin/env bash
#
# Chapter 22 gate: TTIR-min scalar semantics.
#
# Usage: check-ch22.sh <llvm-bin-dir>
#
# What this proves:
#   1. a scalar kernel lowers to TTIR with arith.constant/addi/tt.return;
#   2. the result is accepted by the real Triton parser (triton-opt);
#   3. canonicalised TTIR matches Triton's canonical golden form;
#   4. unsupported statements are rejected with file:line:col;
#   5. tensor/memory calls stop at the chapter-23 boundary with a location.
#
# TRITON_OPT may point at triton-opt.  When it is not available, checks 2 and
# 3 are reported as SKIP; the chapter's own lowering checks still run.
#
set -euo pipefail

BIN=${1:?usage: check-ch22.sh <llvm-bin-dir>}
HERE=$(cd "$(dirname "$0")" && pwd)
TOOL="$BIN/gpumin-triton"
SCALAR="$HERE/triton/scalar_add.py"
GOLDEN="$HERE/triton/scalar_add.ttir"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
export PYTHONDONTWRITEBYTECODE=1

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

[ -x "$TOOL" ] || fail "missing $TOOL"
[ -f "$SCALAR" ] || fail "missing $SCALAR"
[ -f "$GOLDEN" ] || fail "missing $GOLDEN"

TRITON_OPT=${TRITON_OPT:-$(command -v triton-opt || true)}

# --- 1. scalar lowering --------------------------------------------------
"$TOOL" --emit-ttir "$SCALAR" > "$TMP/ours.ttir"
grep -q '^module {$' "$TMP/ours.ttir" ||
  fail "no module"
grep -q 'tt.func public @scalar_add(%n: i32) -> i32 {' "$TMP/ours.ttir" ||
  fail "scalar_add signature is wrong"
grep -q '%0 = arith.constant 1 : i32' "$TMP/ours.ttir" ||
  fail "missing arith.constant"
grep -q '%1 = arith.addi %n, %0 : i32' "$TMP/ours.ttir" ||
  fail "missing arith.addi"
grep -q 'tt.return %1 : i32' "$TMP/ours.ttir" ||
  fail "missing tt.return"
grep -q 'arith.muli\|arith.shrsi\|arith.xori' "$TMP/ours.ttir" ||
  fail "scalar_mix did not lower its arithmetic"
echo "PASS 1/5: scalar expressions lower to TTIR-min"

# --- 2/3. Triton parser and canonical diff -------------------------------
if [ -n "$TRITON_OPT" ] && [ -x "$TRITON_OPT" ]; then
  "$TRITON_OPT" "$TMP/ours.ttir" -o /dev/null ||
    fail "triton-opt rejected our TTIR"
  "$TRITON_OPT" "$TMP/ours.ttir" -o "$TMP/ours.canon.ttir"
  "$TRITON_OPT" "$GOLDEN" -o "$TMP/golden.canon.ttir"
  # The golden covers scalar_add only; compare that function's canonical form.
  awk '/@scalar_add/,/^  }/' "$TMP/ours.canon.ttir" > "$TMP/ours.add"
  awk '/@scalar_add/,/^  }/' "$TMP/golden.canon.ttir" > "$TMP/golden.add"
  diff -u "$TMP/golden.add" "$TMP/ours.add" > "$TMP/canon.diff" ||
    { cat "$TMP/canon.diff" >&2; fail "canonical TTIR differs from Triton"; }
  echo "PASS 2/5: triton-opt parses the TTIR"
  echo "PASS 3/5: canonical TTIR matches the Triton golden form"
else
  echo "SKIP 2/5: triton-opt not found (set TRITON_OPT to enable)"
  echo "SKIP 3/5: canonical diff needs triton-opt"
fi

# --- 4. unsupported statements carry a source location -------------------
cat > "$TMP/bad.py" <<'PY'
import triton
import triton.language as tl

@triton.jit
def bad(n):
    xs = [i for i in range(4)]
    return n + xs[0]
PY
if "$TOOL" --emit-ttir "$TMP/bad.py" > "$TMP/bad.out" 2> "$TMP/bad.err"; then
  fail "a ListComp kernel was accepted"
fi
grep -q 'bad.py:6:9' "$TMP/bad.err" ||
  { cat "$TMP/bad.err" >&2; fail "error lacks file:line:col for ListComp"; }
grep -qi 'ListComp' "$TMP/bad.err" ||
  fail "error does not name the unsupported node"
echo "PASS 4/5: unsupported nodes report file:line:col"

# --- 5. later-chapter boundaries stay explicit -------------------------
cat > "$TMP/boundary.py" <<'PY'
import triton
import triton.language as tl

@triton.jit
def reduce(x_ptr):
    offs = tl.arange(0, 32)
    x = tl.load(x_ptr + offs)
    return tl.max(x, axis=0)
PY
if "$TOOL" --emit-ttir --arg-type 'x_ptr=ptr<f32>' "$TMP/boundary.py" \
      > "$TMP/boundary.out" 2> "$TMP/boundary.err"; then
  fail "tl.max on f32 lowered before f32 select exists"
fi
grep -q 'boundary.py:' "$TMP/boundary.err" ||
  fail "the boundary error lacks a location"
grep -q 'chapter 28' "$TMP/boundary.err" ||
  { cat "$TMP/boundary.err" >&2; fail "the boundary error is not explicit"; }
echo "PASS 5/5: tl.max on f32 reports the chapter-28 boundary"
