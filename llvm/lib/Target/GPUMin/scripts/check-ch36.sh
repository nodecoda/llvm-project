#!/usr/bin/env bash
#
# Chapter 36 gate: 2D mask broadcast and tl.load(other=...).
#
# Usage: check-ch36.sh <llvm-bin-dir>
#
# What this proves:
#   1. the causal kernel carries one three-operand tt.load, the broadcast
#      kernel carries an i1 broadcast to the pointer shape, and both
#      canonicalise to their goldens;
#   2. the masked load lowers to a branch plus a phi whose else value is the
#      frontend's `other` (-1e9 for causal, 0.0 for the broadcast kernel);
#   3. llc still emits the chapter-16 instruction set: five barriers in the
#      causal kernel and no mma;
#   4. both programs match their f32 reference bit-exactly on the simulator;
#   5. a mask that cannot broadcast, a mismatched `other` type and `other`
#      without a mask fail with actionable diagnostics.
#
# TRITON_OPT may point at triton-opt; check 1b is SKIP when it is absent.
#
set -euo pipefail

BIN=${1:?usage: check-ch36.sh <llvm-bin-dir>}
HERE=$(cd "$(dirname "$0")" && pwd)
TOOL="$BIN/gpumin-triton"
LLC="$BIN/llc"
SIM="$HERE/../tools/gpumin_sim.py"
TRITON="$HERE/triton"
CAUSAL="$TRITON/causal.py"
CAUSAL_GOLDEN="$TRITON/causal.ttir"
BROADCAST="$TRITON/mask_broadcast.py"
BROADCAST_GOLDEN="$TRITON/mask_broadcast.ttir"
E2E="$TRITON/e2e"
TMP=$(mktemp -d)
trap 'python3 -c "import shutil,sys; shutil.rmtree(sys.argv[1], ignore_errors=True)" "$TMP"' EXIT
export PYTHONDONTWRITEBYTECODE=1

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

[ -x "$TOOL" ] || fail "missing $TOOL"
[ -x "$LLC" ] || fail "missing $LLC"
[ -f "$CAUSAL" ] || fail "missing $CAUSAL"
[ -f "$CAUSAL_GOLDEN" ] || fail "missing $CAUSAL_GOLDEN"
[ -f "$BROADCAST" ] || fail "missing $BROADCAST"
[ -f "$BROADCAST_GOLDEN" ] || fail "missing $BROADCAST_GOLDEN"
[ -f "$E2E/causal/expected.json" ] || fail "missing e2e/causal"
[ -f "$E2E/mask_broadcast/expected.json" ] || fail "missing e2e/mask_broadcast"

TRITON_OPT=${TRITON_OPT:-$(command -v triton-opt || true)}
EMIT=(--arg-type 'q_ptr=ptr<f32>' --arg-type 'k_ptr=ptr<f32>'
      --arg-type 'v_ptr=ptr<f32>' --arg-type 'p_ptr=ptr<f32>'
      --arg-type 'o_ptr=ptr<f32>')
BEMIT=(--arg-type 'x_ptr=ptr<f32>' --arg-type 'out_ptr=ptr<f32>')

# --- 1. TTIR shape and canonical goldens --------------------------------
"$TOOL" --emit-ttir "${EMIT[@]}" "$CAUSAL" -o "$TMP/causal.ttir"
[ "$(grep -cE '= tt\.load %[^,]+, %[^,]+, %[^ ]+ : tensor<8x8x' "$TMP/causal.ttir")" -eq 1 ] ||
  fail "the causal kernel does not carry exactly one three-operand tt.load"
"$TOOL" --emit-ttir "${BEMIT[@]}" "$BROADCAST" -o "$TMP/broadcast.ttir"
grep -qE 'tt\.broadcast %[^ ]+ : tensor<1x8xi1> -> tensor<8x8xi1>' \
  "$TMP/broadcast.ttir" ||
  fail "the row mask was not broadcast to the pointer shape"
if [ -n "$TRITON_OPT" ] && [ -x "$TRITON_OPT" ]; then
  for pair in "causal:$CAUSAL_GOLDEN" "broadcast:$BROADCAST_GOLDEN"; do
    name=${pair%%:*}
    golden=${pair#*:}
    "$TRITON_OPT" "$TMP/$name.ttir" -o /dev/null ||
      fail "triton-opt rejected the $name TTIR"
    "$TRITON_OPT" "$TMP/$name.ttir" --canonicalize -o "$TMP/$name.canon.ttir"
    "$TRITON_OPT" "$golden" --canonicalize -o "$TMP/$name.golden.ttir"
    diff -u "$TMP/$name.golden.ttir" "$TMP/$name.canon.ttir" > "$TMP/$name.diff" ||
      { cat "$TMP/$name.diff" >&2; fail "$name TTIR differs from the golden"; }
  done
  echo "PASS 1/5: both TTIRs canonicalise to their goldens"
else
  echo "SKIP 1b/5: triton-opt not found (set TRITON_OPT to enable)"
  echo "PASS 1/5: the TTIR carries the masked load and the broadcast mask"
fi

# --- 2. the else value reaches the phi ----------------------------------
"$TOOL" --emit-llvm "${EMIT[@]}" "$CAUSAL" -o "$TMP/causal.ll"
[ "$(grep -c 'phi float' "$TMP/causal.ll")" -eq 2 ] ||
  fail "the two causal elements per thread did not become two phis"
[ "$(grep -c 'br i1' "$TMP/causal.ll")" -eq 2 ] ||
  fail "a masked load did not become a conditional branch"
[ "$(grep -c '\[ -1.000000e+09, %' "$TMP/causal.ll")" -eq 2 ] ||
  fail "the causal phi does not use other = -1e9"
"$TOOL" --emit-llvm "${BEMIT[@]}" "$BROADCAST" -o "$TMP/broadcast.ll"
[ "$(grep -c '\[ 0.000000e+00, %' "$TMP/broadcast.ll")" -eq 2 ] ||
  fail "the broadcast phi does not use other = 0.0"
echo "PASS 2/5: other= lands in the phi's else block"

# --- 3. codegen and the unchanged ISA surface ---------------------------
"$LLC" -march=gpumin -O1 -verify-machineinstrs "$TMP/causal.ll" \
  -o "$TMP/causal.s" || fail "llc rejected the causal kernel"
"$LLC" -march=gpumin -O1 -verify-machineinstrs "$TMP/broadcast.ll" \
  -o "$TMP/broadcast.s" || fail "llc rejected the broadcast kernel"
[ "$(grep -c 'bar.sync' "$TMP/causal.s")" -eq 5 ] ||
  fail "expected 4 reduction barriers + 1 debug barrier in the causal kernel"
if grep -qE '^[[:space:]]+mma' "$TMP/causal.s"; then
  fail "the causal kernel grew an mma instruction"
fi
echo "PASS 3/5: both kernels compile, five barriers, no new instruction"

# --- 4. simulator semantics ---------------------------------------------
python3 "$E2E/run_e2e.py" "$E2E/causal" --bin "$BIN" --sim "$SIM" ||
  fail "the 64 causal outputs mismatched the f32 reference"
python3 "$E2E/run_e2e.py" "$E2E/mask_broadcast" --bin "$BIN" --sim "$SIM" ||
  fail "the 64 broadcast outputs mismatched the f32 reference"
echo "PASS 4/5: causal and broadcast both match their references bit-exactly"

# --- 5. actionable diagnostics ------------------------------------------
cat > "$TMP/badmask.py" <<'PY'
import triton
import triton.language as tl

@triton.jit
def k(p):
    r = tl.arange(0, 8)[:, None]
    c = tl.arange(0, 8)[None, :]
    m = tl.arange(0, 4)[None, :] < 2
    x = tl.load(p + r * 8 + c, mask=m)
    tl.store(p + r * 8 + c, x)
PY
cat > "$TMP/badother.py" <<'PY'
import triton
import triton.language as tl

@triton.jit
def k(p):
    r = tl.arange(0, 8)[:, None]
    c = tl.arange(0, 8)[None, :]
    m = c < 4
    x = tl.load(p + r * 8 + c, mask=m, other=3)
    tl.store(p + r * 8 + c, x)
PY
cat > "$TMP/nomask.py" <<'PY'
import triton
import triton.language as tl

@triton.jit
def k(p):
    r = tl.arange(0, 8)[:, None]
    c = tl.arange(0, 8)[None, :]
    x = tl.load(p + r * 8 + c, other=0.0)
    tl.store(p + r * 8 + c, x)
PY

expect_fail() {
  local what=$1 pattern=$2 file=$3
  if "$TOOL" --emit-ttir --arg-type 'p=ptr<f32>' "$file" \
        > /dev/null 2> "$TMP/neg.err"; then
    fail "$what was accepted"
  fi
  grep -q "$pattern" "$TMP/neg.err" ||
    { cat "$TMP/neg.err" >&2; fail "$what has no actionable diagnostic"; }
}

expect_fail "a mask that cannot broadcast" \
  'mask shape is not broadcast-compatible with the pointer' "$TMP/badmask.py"
expect_fail "a mismatched other type" \
  "other's element type does not match the pointer element type" \
  "$TMP/badother.py"
expect_fail "other without a mask" \
  'tl.load(other=...) needs a mask' "$TMP/nomask.py"
echo "PASS 5/5: the mask and other diagnostics are clear"

echo
echo "Chapter 36: 5/5 checks passed"
