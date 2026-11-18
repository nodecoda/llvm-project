#!/usr/bin/env bash
#
# Chapter 28 gate: elementwise completeness and broadcasting.
#
# Usage: check-ch28.sh <llvm-bin-dir>
#
# What this proves:
#   1. // and % lower to arith.divsi/remsi plus the floor-correction selects,
#      tl.cdiv reuses them, tl.maximum/minimum become cmpi+cmpf/select, and
#      tl.full/tl.zeros become constant+splat; the canonical form matches the
#      golden and folds i32 min/max to arith.minsi/maxsi and the splats to
#      dense constants;
#   2. sdiv/udiv/srem/urem encode and decode round-trip;
#   3. llc selects the integer divides (no i64 magic multiply), the f32
#      ordered compares and SELP, with -verify-machineinstrs;
#   4. the simulator matches Python floor division element by element and
#      reproduces the f32 maximum/minimum and tl.max bit patterns;
#   5. shape mismatches, bad full sizes, f32 cdiv and chained comparisons are
#      rejected with actionable diagnostics.
#
# TRITON_OPT may point at triton-opt; check 1b is SKIP when it is absent.
#
set -euo pipefail

BIN=${1:?usage: check-ch28.sh <llvm-bin-dir>}
HERE=$(cd "$(dirname "$0")" && pwd)
TOOL="$BIN/gpumin-triton"
LLC="$BIN/llc"
MC="$BIN/llvm-mc"
SIM="$HERE/../tools/gpumin_sim.py"
SRC="$HERE/triton/elementwise.py"
GOLDEN="$HERE/triton/elementwise.ttir"
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

TRITON_OPT=${TRITON_OPT:-$(command -v triton-opt || true)}

EMIT=(--constexpr BLOCK=32 --arg-type out_ptr=ptr
      --arg-type 'fx_ptr=ptr<f32>' --arg-type 'fy_ptr=ptr<f32>'
      --arg-type 'fout_ptr=ptr<f32>')

# --- 1. TTIR shape and canonical golden ---------------------------------
"$TOOL" --emit-ttir "${EMIT[@]}" "$SRC" -o "$TMP/ew.ttir"
grep -Eq 'arith.divsi .* : tensor<32xi32>' "$TMP/ew.ttir" ||
  fail "// did not lower to arith.divsi"
grep -Eq 'arith.remsi .* : tensor<32xi32>' "$TMP/ew.ttir" ||
  fail "% did not lower to arith.remsi"
grep -Eq 'arith.select .* : tensor<32xi1>, tensor<32xi32>' "$TMP/ew.ttir" ||
  fail "the floor correction is not a typed tensor select"
grep -Eq 'arith.cmpf ogt, .* : tensor<32xf32>' "$TMP/ew.ttir" ||
  fail "f32 maximum is not an ordered greater-than compare"
grep -Eq 'arith.cmpf olt, .* : tensor<32xf32>' "$TMP/ew.ttir" ||
  fail "f32 minimum is not an ordered less-than compare"
grep -q 'arith.maximumf' "$TMP/ew.ttir" ||
  fail "the f32 tl.max combiner is not arith.maximumf"
grep -q 'tt.splat' "$TMP/ew.ttir" || fail "full/zeros did not splat"
if [ -n "$TRITON_OPT" ] && [ -x "$TRITON_OPT" ]; then
  "$TRITON_OPT" "$TMP/ew.ttir" -o /dev/null ||
    fail "triton-opt rejected the elementwise TTIR"
  "$TRITON_OPT" "$TMP/ew.ttir" --canonicalize -o "$TMP/ours.canon.ttir"
  "$TRITON_OPT" "$GOLDEN" --canonicalize -o "$TMP/golden.canon.ttir"
  diff -u "$TMP/golden.canon.ttir" "$TMP/ours.canon.ttir" \
    > "$TMP/canon.diff" ||
    { cat "$TMP/canon.diff" >&2; fail "canonical TTIR differs from the golden"; }
  grep -q 'arith.maxsi' "$TMP/ours.canon.ttir" ||
    fail "i32 maximum did not fold to arith.maxsi"
  grep -q 'arith.minsi' "$TMP/ours.canon.ttir" ||
    fail "i32 minimum did not fold to arith.minsi"
  grep -q 'dense<7>' "$TMP/ours.canon.ttir" ||
    fail "tl.full did not fold to a dense constant"
  echo "PASS 1/5: elementwise TTIR canonicalises to the golden"
else
  echo "SKIP 1b/5: triton-opt not found (set TRITON_OPT to enable)"
  echo "PASS 1/5: elementwise TTIR has the expected shape"
fi

# --- 2. MC round-trip ----------------------------------------------------
cat > "$TMP/div.s" <<'ASM'
sdiv r4, r5, r6
udiv r7, r8, r9
srem r10, r11, r12
urem r13, r14, r15
ASM
"$MC" -triple=gpumin -show-encoding "$TMP/div.s" |
  sed -n 's/.*encoding: \[\(.*\)\]/\1/p' > "$TMP/bytes.txt"
[ "$(wc -l < "$TMP/bytes.txt")" -eq 4 ] ||
  fail "expected four encodings"
"$MC" -triple=gpumin -disassemble "$TMP/bytes.txt" > "$TMP/round.s"
for op in sdiv udiv srem urem; do
  grep -qE "^[[:space:]]*$op r" "$TMP/round.s" ||
    fail "$op did not round-trip"
done
echo "PASS 2/5: sdiv/udiv/srem/urem encode and decode"

# --- 3. codegen ----------------------------------------------------------
"$TOOL" --emit-llvm "${EMIT[@]}" "$SRC" -o "$TMP/ew.ll"
"$LLC" -march=gpumin -O1 -verify-machineinstrs "$TMP/ew.ll" -o "$TMP/ew.s" ||
  fail "llc rejected the elementwise kernel"
grep -Eq '^[[:space:]]+sdiv ' "$TMP/ew.s" || fail "no sdiv in the assembly"
grep -Eq '^[[:space:]]+srem ' "$TMP/ew.s" || fail "no srem in the assembly"
if grep -q 'mulhi' "$TMP/ew.s"; then
  fail "division by a constant was rewritten to an i64 magic multiply"
fi
grep -q 'fsetp.ogt' "$TMP/ew.s" ||
  fail "f32 maximum did not select fsetp.ogt"
grep -q 'fsetp.olt' "$TMP/ew.s" ||
  fail "f32 minimum did not select fsetp.olt"
grep -q 'selp' "$TMP/ew.s" || fail "the f32 min/max did not select SELP"
echo "PASS 3/5: llc emits integer divides, f32 compares and SELP"

# --- 4. simulator semantics ---------------------------------------------
"$LLC" -march=gpumin -O1 -verify-machineinstrs -filetype=obj \
  "$TMP/ew.ll" -o "$TMP/ew.o" || fail "llc rejected the elementwise object"
cat > "$TMP/alu.json" <<'JSON'
{
  "grid": 1,
  "block": 32,
  "args_addr": 4096,
  "args": [0],
  "dump": [ { "addr": 0, "count": 32 } ]
}
JSON
want=$(python3 - <<'PY'
vals = []
for off in range(32):
    a, b = off - 16, 3
    q, r = a // b, a % b
    c = -((-a) // b)
    vals.append(q + r + c + max(a, 0) + min(a, 0) + 0 + 7)
print(" ".join(str(v) for v in vals))
PY
)
got=$(python3 "$SIM" "$TMP/ew.o" --kernel alu_kernel --launch "$TMP/alu.json") ||
  fail "the simulator rejected alu_kernel"
[ "$got" = "$want" ] ||
  fail "floor division/cdiv/min/max mismatch:
  got  $got
  want $want"

cat > "$TMP/fminmax.json" <<'JSON'
{
  "grid": 1,
  "block": 32,
  "args_addr": 4096,
  "args": [0, 8192, 16384],
  "fill": [
    { "addr": 0, "start": 1069547520, "step": 0, "count": 32 },
    { "addr": 8192, "start": 1075838976, "step": 0, "count": 32 }
  ],
  "dump": [ { "addr": 16384, "count": 2 } ]
}
JSON
got=$(python3 "$SIM" "$TMP/ew.o" --kernel fminmax_kernel \
        --launch "$TMP/fminmax.json") ||
  fail "the simulator rejected fminmax_kernel"
[ "$got" = "1065353216 1065353216" ] ||
  fail "f32 maximum-minimum produced '$got', want the bits of 1.0"

cat > "$TMP/fmax.json" <<'JSON'
{
  "grid": 1,
  "block": 32,
  "args_addr": 4096,
  "args": [0, 8192],
  "fill": [
    { "addr": 0, "start": 1069547520, "step": 0, "count": 16 },
    { "addr": 64, "start": 1075838976, "step": 0, "count": 16 }
  ],
  "dump": [ { "addr": 8192, "count": 1 } ]
}
JSON
got=$(python3 "$SIM" "$TMP/ew.o" --kernel fmax_kernel --launch "$TMP/fmax.json") ||
  fail "the simulator rejected f32 tl.max"
[ "$got" = "1075838976" ] ||
  fail "f32 tl.max produced '$got', want the bits of 2.5"
echo "PASS 4/5: floor division, f32 min/max and f32 tl.max match references"

# --- 5. actionable diagnostics ------------------------------------------
cat > "$TMP/mismatch.py" <<'PY'
import triton
import triton.language as tl

@triton.jit
def k(p):
    a = tl.arange(0, 32)
    b = tl.arange(0, 64)
    v = tl.maximum(a, b)
    tl.store(p, v)
PY
if "$TOOL" --emit-ttir --arg-type p=ptr "$TMP/mismatch.py" \
      > /dev/null 2> "$TMP/mismatch.err"; then
  fail "tl.maximum accepted mismatched shapes"
fi
grep -q 'tensor type mismatch' "$TMP/mismatch.err" ||
  { cat "$TMP/mismatch.err" >&2; fail "shape-mismatch diagnostic is not actionable"; }

cat > "$TMP/badfull.py" <<'PY'
import triton
import triton.language as tl

@triton.jit
def k(p):
    v = tl.full((16,), 7, tl.int32)
    tl.store(p, v)
PY
if "$TOOL" --emit-ttir --arg-type p=ptr "$TMP/badfull.py" \
      > /dev/null 2> "$TMP/badfull.err"; then
  fail "tl.full accepted a non-multiple-of-32 size"
fi
grep -q 'positive multiple of 32' "$TMP/badfull.err" ||
  { cat "$TMP/badfull.err" >&2; fail "full-size diagnostic is not actionable"; }

cat > "$TMP/fdiv.py" <<'PY'
import triton
import triton.language as tl

@triton.jit
def k(p):
    offs = tl.arange(0, 32)
    x = tl.load(p + offs)
    v = tl.cdiv(x, x)
    tl.store(p, v)
PY
if "$TOOL" --emit-ttir --arg-type 'p=ptr<f32>' "$TMP/fdiv.py" \
      > /dev/null 2> "$TMP/fdiv.err"; then
  fail "tl.cdiv accepted f32 operands"
fi
grep -q 'tl.cdiv requires integer operands' "$TMP/fdiv.err" ||
  { cat "$TMP/fdiv.err" >&2; fail "cdiv diagnostic is not actionable"; }

cat > "$TMP/chain.py" <<'PY'
import triton
import triton.language as tl

@triton.jit
def k(p):
    a = tl.arange(0, 32)
    v = 1 < a < 5
    tl.store(p, v)
PY
if "$TOOL" --emit-ttir --arg-type p=ptr "$TMP/chain.py" \
      > /dev/null 2> "$TMP/chain.err"; then
  fail "a chained comparison was accepted"
fi
grep -q 'chained comparisons are not supported' "$TMP/chain.err" ||
  { cat "$TMP/chain.err" >&2; fail "chained-comparison diagnostic is not actionable"; }
echo "PASS 5/5: bad shapes, full sizes, f32 cdiv and chains are rejected"

echo
echo "Chapter 28: 5/5 checks passed"
