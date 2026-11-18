#!/usr/bin/env bash
#
# Chapter 23 gate: tensor, pointer and mask semantics.
#
# Usage: check-ch23.sh <llvm-bin-dir>
#
# What this proves:
#   1. add_i32.py lowers to the full TTIR op sequence;
#   2. pointer arithmetic becomes tt.addptr (never integer addi);
#   3. masked loads/stores carry the same tensor<i1> mask;
#   4. triton-opt parses the result and the canonical form matches the golden;
#   5. missing tl.constexpr bindings are rejected with an actionable error.
#
# TRITON_OPT may point at triton-opt; checks 4a/4b are SKIP when it is absent.
#
set -euo pipefail

BIN=${1:?usage: check-ch23.sh <llvm-bin-dir>}
HERE=$(cd "$(dirname "$0")" && pwd)
TOOL="$BIN/gpumin-triton"
ADD="$HERE/triton/add_i32.py"
GOLDEN="$HERE/triton/add_i32.ttir"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
export PYTHONDONTWRITEBYTECODE=1

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

[ -x "$TOOL" ] || fail "missing $TOOL"
[ -f "$ADD" ] || fail "missing $ADD"
[ -f "$GOLDEN" ] || fail "missing $GOLDEN"

TRITON_OPT=${TRITON_OPT:-$(command -v triton-opt || true)}

EMIT=(--emit-ttir --constexpr BLOCK=1024
      --arg-type x_ptr=ptr --arg-type y_ptr=ptr --arg-type out_ptr=ptr)

# --- 1. full op sequence -------------------------------------------------
"$TOOL" "${EMIT[@]}" "$ADD" > "$TMP/ours.ttir"
grep -q 'tt.func public @add_kernel(%x_ptr: !tt.ptr<i32>, %y_ptr: !tt.ptr<i32>, %out_ptr: !tt.ptr<i32>, %n: i32)' "$TMP/ours.ttir" ||
  fail "kernel signature is wrong"
grep -q 'tt.get_program_id x : i32' "$TMP/ours.ttir" ||
  fail "missing tt.get_program_id"
grep -q 'tt.make_range {start = 0 : i32, end = 1024 : i32} : tensor<1024xi32>' "$TMP/ours.ttir" ||
  fail "missing tt.make_range"
grep -q 'tt.splat' "$TMP/ours.ttir" ||
  fail "missing tt.splat"
grep -q 'arith.cmpi slt, .* : tensor<1024xi32>' "$TMP/ours.ttir" ||
  fail "missing tensor comparison"
[ "$(grep -c 'tt.load ' "$TMP/ours.ttir")" -eq 2 ] ||
  fail "expected two tt.load ops"
grep -q 'tt.store ' "$TMP/ours.ttir" ||
  fail "missing tt.store"
grep -q 'arith.addi %x, %y : tensor<1024xi32>' "$TMP/ours.ttir" ||
  fail "missing tensor addition"
echo "PASS 1/5: add_i32.py lowers to the full TTIR sequence"

# --- 2. pointer arithmetic is tt.addptr ----------------------------------
addptrs=$(grep -c 'tt.addptr ' "$TMP/ours.ttir" || true)
[ "$addptrs" -eq 3 ] ||
  fail "expected three tt.addptr ops, found $addptrs"
if grep -q 'arith.addi %x_ptr' "$TMP/ours.ttir"; then
  fail "pointer arithmetic was lowered as integer addi"
fi
echo "PASS 2/5: pointer arithmetic uses tt.addptr"

# --- 3. loads and stores share the mask ----------------------------------
mask=$(awk '/arith.cmpi slt/ {print $1}' "$TMP/ours.ttir")
[ -n "$mask" ] || fail "no mask value"
[ "$(grep -c "tt.load .*, ${mask} :" "$TMP/ours.ttir")" -eq 2 ] ||
  fail "loads do not use the mask"
grep -q "tt.store .*, ${mask} :" "$TMP/ours.ttir" ||
  fail "store does not use the mask"
echo "PASS 3/5: loads and stores carry the tensor mask"

# --- 4. Triton parser and canonical golden -------------------------------
if [ -n "$TRITON_OPT" ] && [ -x "$TRITON_OPT" ]; then
  "$TRITON_OPT" "$TMP/ours.ttir" -o /dev/null ||
    fail "triton-opt rejected our TTIR"
  "$TRITON_OPT" "$TMP/ours.ttir" -o "$TMP/ours.canon.ttir"
  "$TRITON_OPT" "$GOLDEN" -o "$TMP/golden.canon.ttir"
  diff -u "$TMP/golden.canon.ttir" "$TMP/ours.canon.ttir" > "$TMP/canon.diff" ||
    { cat "$TMP/canon.diff" >&2; fail "canonical TTIR differs from the golden"; }
  echo "PASS 4/5: triton-opt parses the TTIR and it matches the golden"
else
  echo "SKIP 4/5: triton-opt not found (set TRITON_OPT to enable)"
fi

# --- 5. missing constexpr bindings are actionable ------------------------
if "$TOOL" --emit-ttir --arg-type x_ptr=ptr --arg-type y_ptr=ptr \
      --arg-type out_ptr=ptr "$ADD" > "$TMP/missing.out" 2> "$TMP/missing.err"; then
  fail "a kernel with an unbound constexpr argument was accepted"
fi
grep -q "constexpr argument 'BLOCK' has no value" "$TMP/missing.err" ||
  { cat "$TMP/missing.err" >&2; fail "missing-constexpr error is not actionable"; }
grep -q -- '--constexpr BLOCK=' "$TMP/missing.err" ||
  fail "missing-constexpr error does not show the fix"
echo "PASS 5/5: unbound tl.constexpr arguments are reported with a fix"
