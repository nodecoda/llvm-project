#!/usr/bin/env bash
#
# Chapter 31 gate: 2D reductions and row-wise softmax.
#
# Usage: check-ch31.sh <llvm-bin-dir>
#
# What this proves:
#   1. the softmax kernel uses two axis-1 tt.reduce ops, math.exp2 and
#      arith.divf, and the TTIR canonicalises to the golden;
#   2. the new ISA surface, fdiv (FALU funct 3) and exp2 (SFU opcode 0x0B,
#      funct 0), encodes and decodes round-trip;
#   3. llc lowers both reductions through shared memory: bar.sync, st.shared,
#      ld.shared, fdiv and exp2 are all present;
#   4. the simulator reproduces an 8x8 row softmax bit-exactly and every row
#      sums to one;
#   5. missing keep_dims, an out-of-range axis, an integer '/', and a
#      non-f32 tl.exp2 fail with actionable diagnostics.
#
# TRITON_OPT may point at triton-opt; check 1b is SKIP when it is absent.
#
set -euo pipefail

BIN=${1:?usage: check-ch31.sh <llvm-bin-dir>}
HERE=$(cd "$(dirname "$0")" && pwd)
TOOL="$BIN/gpumin-triton"
LLC="$BIN/llc"
MC="$BIN/llvm-mc"
SIM="$HERE/../tools/gpumin_sim.py"
SRC="$HERE/triton/softmax2d.py"
GOLDEN="$HERE/triton/softmax2d.ttir"
E2E="$HERE/triton/e2e/softmax2d"
TMP=$(mktemp -d)
trap 'python3 -c "import shutil,sys; shutil.rmtree(sys.argv[1], ignore_errors=True)" "$TMP"' EXIT
export PYTHONDONTWRITEBYTECODE=1

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

[ -x "$TOOL" ] || fail "missing $TOOL"
[ -x "$LLC" ] || fail "missing $LLC"
[ -x "$MC" ] || fail "missing $MC"
[ -f "$SRC" ] || fail "missing $SRC"
[ -f "$GOLDEN" ] || fail "missing $GOLDEN"
[ -f "$E2E/expected.json" ] || fail "missing $E2E/expected.json"

TRITON_OPT=${TRITON_OPT:-$(command -v triton-opt || true)}
EMIT=(--arg-type 'x_ptr=ptr<f32>')

# --- 1. TTIR shape and canonical golden ---------------------------------
"$TOOL" --emit-ttir "${EMIT[@]}" "$SRC" -o "$TMP/sm.ttir"
grep -q 'tt.reduce.*axis = 1 : i32' "$TMP/sm.ttir" ||
  fail "the row reductions do not use axis 1"
[ "$(grep -c '= "tt.reduce"' "$TMP/sm.ttir")" -eq 2 ] ||
  fail "expected two tt.reduce ops (max and sum)"
grep -q ': (tensor<8x8xf32>) -> tensor<8xf32>' "$TMP/sm.ttir" ||
  fail "a 2D reduction did not drop the reduced dimension"
grep -q 'tt.expand_dims .* : tensor<8xf32> -> tensor<8x1xf32>' "$TMP/sm.ttir" ||
  fail "keep_dims=True did not expand back to [8,1]"
grep -q 'math.exp2 .* : tensor<8x8xf32>' "$TMP/sm.ttir" ||
  fail "tl.exp2 did not become math.exp2"
grep -q 'arith.divf .* : tensor<8x8xf32>' "$TMP/sm.ttir" ||
  fail "the normalization did not become arith.divf"
if [ -n "$TRITON_OPT" ] && [ -x "$TRITON_OPT" ]; then
  "$TRITON_OPT" "$TMP/sm.ttir" -o /dev/null ||
    fail "triton-opt rejected the softmax TTIR"
  "$TRITON_OPT" "$TMP/sm.ttir" --canonicalize -o "$TMP/ours.canon.ttir"
  "$TRITON_OPT" "$GOLDEN" --canonicalize -o "$TMP/golden.canon.ttir"
  diff -u "$TMP/golden.canon.ttir" "$TMP/ours.canon.ttir" > "$TMP/canon.diff" ||
    { cat "$TMP/canon.diff" >&2; fail "canonical TTIR differs from the golden"; }
  echo "PASS 1/5: 2D reduce/exp2/divf canonicalise to the golden"
else
  echo "SKIP 1b/5: triton-opt not found (set TRITON_OPT to enable)"
  echo "PASS 1/5: the softmax TTIR has the expected 2D reduction shape"
fi

# --- 2. ISA round-trip ---------------------------------------------------
cat > "$TMP/f32.s" <<'ASM'
fdiv r16, r17, r18
exp2 r16, r17
ASM
"$MC" -triple=gpumin -show-encoding "$TMP/f32.s" |
  sed -n 's/.*encoding: \[\(.*\)\]/\1/p' > "$TMP/bytes.txt"
[ "$(wc -l < "$TMP/bytes.txt")" -eq 2 ] || fail "expected two encodings"
grep -q '^0x03,0x90,0x11,0x1e$' "$TMP/bytes.txt" ||
  fail "fdiv does not encode as FALU funct 3"
grep -q '^0x00,0x00,0x11,0x2e$' "$TMP/bytes.txt" ||
  fail "exp2 does not encode as SFU opcode 0x0B funct 0"
"$MC" -triple=gpumin -disassemble "$TMP/bytes.txt" > "$TMP/round.s"
grep -q 'fdiv r16, r17, r18' "$TMP/round.s" || fail "fdiv did not round-trip"
grep -q 'exp2 r16, r17' "$TMP/round.s" || fail "exp2 did not round-trip"
echo "PASS 2/5: fdiv and exp2 encode and decode"

# --- 3. codegen ----------------------------------------------------------
"$TOOL" --emit-llvm "${EMIT[@]}" "$SRC" -o "$TMP/sm.ll"
"$LLC" -march=gpumin -O1 -verify-machineinstrs "$TMP/sm.ll" \
  -o "$TMP/sm.s" || fail "llc rejected the softmax kernel"
grep -q 'st.shared' "$TMP/sm.s" || fail "the tile never reaches shared memory"
grep -q 'ld.shared' "$TMP/sm.s" || fail "the reductions do not read shared memory"
barriers=$(grep -c 'bar.sync' "$TMP/sm.s")
[ "$barriers" -ge 2 ] || fail "expected a barrier per reduction, got $barriers"
grep -qE '^[[:space:]]+fdiv ' "$TMP/sm.s" ||
  fail "the normalization did not select fdiv"
grep -qE '^[[:space:]]+exp2 ' "$TMP/sm.s" ||
  fail "tl.exp2 did not select the SFU"
echo "PASS 3/5: both reductions use shared memory, fdiv and exp2 run in asm"

# --- 4. simulator semantics ---------------------------------------------
python3 "$HERE/triton/e2e/run_e2e.py" "$E2E" --bin "$BIN" --sim "$SIM" ||
  fail "the 8x8 softmax mismatched the f32 reference"
echo "PASS 4/5: the 8x8 row softmax matches the f32 reference"

# --- 5. actionable diagnostics ------------------------------------------
cat > "$TMP/nokeep.py" <<'PY'
import triton
import triton.language as tl

@triton.jit
def k(p):
    r = tl.arange(0, 8)[:, None]
    c = tl.arange(0, 8)[None, :]
    x = tl.load(p + r * 8 + c)
    s = tl.sum(x, axis=1)
    tl.store(p, s)
PY
if "$TOOL" --emit-ttir --arg-type p=ptr "$TMP/nokeep.py" \
      > /dev/null 2> "$TMP/nokeep.err"; then
  fail "a 2D reduction without keep_dims was accepted"
fi
grep -q 'keep_dims=True' "$TMP/nokeep.err" ||
  { cat "$TMP/nokeep.err" >&2; fail "the keep_dims diagnostic is not actionable"; }

cat > "$TMP/axis2.py" <<'PY'
import triton
import triton.language as tl

@triton.jit
def k(p):
    r = tl.arange(0, 8)[:, None]
    c = tl.arange(0, 8)[None, :]
    x = tl.load(p + r * 8 + c)
    s = tl.sum(x, axis=2, keep_dims=True)
    tl.store(p, s)
PY
if "$TOOL" --emit-ttir --arg-type p=ptr "$TMP/axis2.py" \
      > /dev/null 2> "$TMP/axis2.err"; then
  fail "axis=2 was accepted on a 2D tensor"
fi
grep -q 'axis 0 or 1' "$TMP/axis2.err" ||
  { cat "$TMP/axis2.err" >&2; fail "the axis diagnostic is not actionable"; }

cat > "$TMP/exp2i.py" <<'PY'
import triton
import triton.language as tl

@triton.jit
def k(p):
    x = tl.arange(0, 8)
    y = tl.exp2(x)
    tl.store(p + x, y)
PY
if "$TOOL" --emit-ttir --arg-type p=ptr "$TMP/exp2i.py" \
      > /dev/null 2> "$TMP/exp2i.err"; then
  fail "tl.exp2 on an integer tensor was accepted"
fi
grep -q 'tl.exp2 requires an f32 tensor' "$TMP/exp2i.err" ||
  { cat "$TMP/exp2i.err" >&2; fail "the exp2 diagnostic is not actionable"; }

cat > "$TMP/intdiv.py" <<'PY'
import triton
import triton.language as tl

@triton.jit
def k(p):
    x = tl.load(p + tl.arange(0, 8))
    y = x / 2
    tl.store(p + tl.arange(0, 8), y)
PY
if "$TOOL" --emit-ttir --arg-type p=ptr "$TMP/intdiv.py" \
      > /dev/null 2> "$TMP/intdiv.err"; then
  fail "integer '/' was accepted as true division"
fi
grep -q "requires f32 operands" "$TMP/intdiv.err" ||
  { cat "$TMP/intdiv.err" >&2; fail "the integer division diagnostic is not actionable"; }
echo "PASS 5/5: keep_dims, axis, exp2 and integer '/' diagnostics are clear"

echo
echo "Chapter 31: 5/5 checks passed"
