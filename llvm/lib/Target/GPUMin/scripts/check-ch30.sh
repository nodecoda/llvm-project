#!/usr/bin/env bash
#
# Chapter 30 gate: 2D tiled matmul without tl.dot.
#
# Usage: check-ch30.sh <llvm-bin-dir>
#
# What this proves:
#   1. the rank-1 update kernel emits 2D zeros, both broadcasts, mulf/addf and
#      two grid axes, and canonicalises to the golden;
#   2. the chapter-29 ISA surface is unchanged: mul still assembles to funct
#      0x014 and the kernel itself uses fmul/fadd;
#   3. llc compiles the 32x32x32 kernel with -verify-machineinstrs and the
#      assembly reads sr.blockidx/sr.blockidy and global memory;
#   4. the simulator reproduces the 32x32x32 product for a 4x4 grid of 8x8
#      tiles (1024 f32 outputs);
#   5. a 1D tl.dot (chapter 35 only supports 2D) and illegal 2D zero shapes
#      fail with actionable diagnostics.
#
# TRITON_OPT may point at triton-opt; check 1b is SKIP when it is absent.
#
set -euo pipefail

BIN=${1:?usage: check-ch30.sh <llvm-bin-dir>}
HERE=$(cd "$(dirname "$0")" && pwd)
TOOL="$BIN/gpumin-triton"
LLC="$BIN/llc"
MC="$BIN/llvm-mc"
SIM="$HERE/../tools/gpumin_sim.py"
SRC="$HERE/triton/matmul2d.py"
GOLDEN="$HERE/triton/matmul2d.ttir"
E2E="$HERE/triton/e2e/matmul2d"
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
EMIT=(--arg-type 'a_ptr=ptr<f32>' --arg-type 'b_ptr=ptr<f32>'
      --arg-type 'c_ptr=ptr<f32>')

# --- 1. TTIR shape and canonical golden ---------------------------------
"$TOOL" --emit-ttir "${EMIT[@]}" "$SRC" -o "$TMP/m2.ttir"
grep -q 'tt.splat .* : f32 -> tensor<8x8xf32>' "$TMP/m2.ttir" ||
  fail "tl.zeros((8, 8), tl.float32) did not become a 2D splat"
grep -q 'tt.get_program_id x : i32' "$TMP/m2.ttir" ||
  fail "program_id(0) is missing"
grep -q 'tt.get_program_id y : i32' "$TMP/m2.ttir" ||
  fail "program_id(1) did not become get_program_id y"
grep -q 'tt.broadcast .* : tensor<8x1xf32> -> tensor<8x8xf32>' \
  "$TMP/m2.ttir" || fail "the A column was not broadcast"
grep -q 'tt.broadcast .* : tensor<1x8xf32> -> tensor<8x8xf32>' \
  "$TMP/m2.ttir" || fail "the B row was not broadcast"
grep -q 'arith.mulf .* : tensor<8x8xf32>' "$TMP/m2.ttir" ||
  fail "the outer product did not become mulf"
grep -q 'arith.addf .* : tensor<8x8xf32>' "$TMP/m2.ttir" ||
  fail "the accumulator update did not become addf"
if [ -n "$TRITON_OPT" ] && [ -x "$TRITON_OPT" ]; then
  "$TRITON_OPT" "$TMP/m2.ttir" -o /dev/null ||
    fail "triton-opt rejected the matmul TTIR"
  "$TRITON_OPT" "$TMP/m2.ttir" --canonicalize -o "$TMP/ours.canon.ttir"
  "$TRITON_OPT" "$GOLDEN" --canonicalize -o "$TMP/golden.canon.ttir"
  diff -u "$TMP/golden.canon.ttir" "$TMP/ours.canon.ttir" \
    > "$TMP/canon.diff" ||
    { cat "$TMP/canon.diff" >&2; fail "canonical TTIR differs from the golden"; }
  grep -q 'dense<0.000000e+00> : tensor<8x8xf32>' "$TMP/ours.canon.ttir" ||
    fail "tl.zeros did not fold to a dense f32 zero"
  echo "PASS 1/5: the rank-1 update canonicalises to the golden"
else
  echo "SKIP 1b/5: triton-opt not found (set TRITON_OPT to enable)"
  echo "PASS 1/5: the matmul TTIR has the expected rank-1 shape"
fi

# --- 2. ISA surface ------------------------------------------------------
got=$(printf 'mul r16, r17, r18\n' |
      "$MC" -triple=gpumin -show-encoding |
      grep -o '\[[^]]*\]' | tr -d '[]')
[ "$got" = "0x14,0x90,0x11,0x0a" ] || fail "mul encodes as [$got]"
echo "PASS 2/5: mul still encodes as ALU funct 0x014"

# --- 3. codegen ----------------------------------------------------------
"$TOOL" --emit-llvm "${EMIT[@]}" "$SRC" -o "$TMP/m2.ll"
"$LLC" -march=gpumin -O1 -verify-machineinstrs "$TMP/m2.ll" \
  -o "$TMP/m2.s" || fail "llc rejected the 32x32x32 kernel"
grep -q 'sr.blockidx' "$TMP/m2.s" || fail "no sr.blockidx in the assembly"
grep -q 'sr.blockidy' "$TMP/m2.s" || fail "no sr.blockidy in the assembly"
grep -qE '^[[:space:]]+fmul ' "$TMP/m2.s" ||
  fail "the rank-1 update did not select fmul"
grep -qE '^[[:space:]]+fadd ' "$TMP/m2.s" ||
  fail "the accumulator update did not select fadd"
grep -q 'ld.global' "$TMP/m2.s" || fail "the kernel does not read global memory"
grep -q 'st.global' "$TMP/m2.s" || fail "the kernel does not write global memory"
echo "PASS 3/5: codegen uses 2D grid registers, fmul/fadd and global memory"

# --- 4. simulator semantics ---------------------------------------------
python3 "$HERE/triton/e2e/run_e2e.py" "$E2E" --bin "$BIN" --sim "$SIM" ||
  fail "the 32x32x32 matmul mismatched the reference"
echo "PASS 4/5: the 4x4 grid of 8x8 tiles matches the closed form"

# --- 5. actionable diagnostics ------------------------------------------
cat > "$TMP/dot.py" <<'PY'
import triton
import triton.language as tl

@triton.jit
def k(p):
    x = tl.load(p + tl.arange(0, 8))
    y = tl.load(p + tl.arange(0, 8))
    z = tl.dot(x, y)
    tl.store(p + tl.arange(0, 8), z)
PY
if "$TOOL" --emit-ttir --arg-type p=ptr "$TMP/dot.py" \
      > /dev/null 2> "$TMP/dot.err"; then
  fail "a 1D tl.dot was accepted"
fi
grep -q 'tl.dot requires 2D f32 tiles' "$TMP/dot.err" ||
  { cat "$TMP/dot.err" >&2; fail "the tl.dot diagnostic is not actionable"; }

cat > "$TMP/big.py" <<'PY'
import triton
import triton.language as tl

@triton.jit
def k(p):
    z = tl.zeros((32, 32), tl.float32)
    r = tl.arange(0, 32)[:, None]
    c = tl.arange(0, 32)[None, :]
    tl.store(p + r * 32 + c, z)
PY
if "$TOOL" --emit-llvm --arg-type p=ptr "$TMP/big.py" \
      > /dev/null 2> "$TMP/big.err"; then
  fail "a 32x32 zero tile was accepted"
fi
grep -q 'at most 8 elements per thread' "$TMP/big.err" ||
  { cat "$TMP/big.err" >&2; fail "the tile-size diagnostic is not actionable"; }

cat > "$TMP/col.py" <<'PY'
import triton
import triton.language as tl

@triton.jit
def k(p):
    z = tl.zeros((8, 9), tl.float32)
    tl.store(p, 0.0)
PY
if "$TOOL" --emit-llvm --arg-type p=ptr "$TMP/col.py" \
      > /dev/null 2> "$TMP/col.err"; then
  fail "an 8x9 zero tile was accepted"
fi
grep -q 'columns must be 1 or a multiple of 8' "$TMP/col.err" ||
  { cat "$TMP/col.err" >&2; fail "the column diagnostic is not actionable"; }
echo "PASS 5/5: a 1D tl.dot, oversized tiles and bad columns fail with messages"

echo
echo "Chapter 30: 5/5 checks passed"
