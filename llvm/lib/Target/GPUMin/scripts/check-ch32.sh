#!/usr/bin/env bash
#
# Chapter 32 gate: the third-course capstone (8x8 single-head attention).
#
# Usage: check-ch32.sh <llvm-bin-dir>
#
# What this proves:
#   1. the kernel has two unrolled rank-1 loops, two axis-1 tt.reduce ops,
#      math.exp2, arith.divf and ttg.barrier all, and canonicalises to the
#      golden;
#   2. the ISA gains nothing: fdiv and exp2 still encode and decode;
#   3. llc emits the P stores before the debug barrier and the P reloads
#      after it, and the kernel passes -verify-machineinstrs;
#   4. the simulator produces the 64 attention outputs bit-exactly;
#   5. tl.debug_barrier with an argument and a computed-tile column slice
#      fail with actionable diagnostics.
#
# TRITON_OPT may point at triton-opt; check 1b is SKIP when it is absent.
#
set -euo pipefail

BIN=${1:?usage: check-ch32.sh <llvm-bin-dir>}
HERE=$(cd "$(dirname "$0")" && pwd)
TOOL="$BIN/gpumin-triton"
LLC="$BIN/llc"
MC="$BIN/llvm-mc"
SIM="$HERE/../tools/gpumin_sim.py"
SRC="$HERE/triton/attention.py"
GOLDEN="$HERE/triton/attention.ttir"
E2E="$HERE/triton/e2e/attention"
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
EMIT=(--arg-type 'q_ptr=ptr<f32>' --arg-type 'k_ptr=ptr<f32>'
      --arg-type 'v_ptr=ptr<f32>' --arg-type 'p_ptr=ptr<f32>'
      --arg-type 'o_ptr=ptr<f32>')

# --- 1. TTIR shape and canonical golden ---------------------------------
"$TOOL" --emit-ttir "${EMIT[@]}" "$SRC" -o "$TMP/attn.ttir"
[ "$(grep -c 'arith.mulf' "$TMP/attn.ttir")" -ge 16 ] ||
  fail "the two rank-1 loops did not unroll to sixteen products"
[ "$(grep -c 'arith.addf' "$TMP/attn.ttir")" -ge 16 ] ||
  fail "the two rank-1 loops did not unroll to sixteen accumulations"
[ "$(grep -c '= "tt.reduce"' "$TMP/attn.ttir")" -eq 2 ] ||
  fail "expected two tt.reduce ops (row max and row sum)"
grep -q 'tt.reduce.*axis = 1 : i32' "$TMP/attn.ttir" ||
  fail "the reductions do not use axis 1"
grep -q 'math.exp2 .* : tensor<8x8xf32>' "$TMP/attn.ttir" ||
  fail "the softmax did not become math.exp2"
grep -q 'arith.divf .* : tensor<8x8xf32>' "$TMP/attn.ttir" ||
  fail "the normalization did not become arith.divf"
grep -q '^    ttg.barrier all$' "$TMP/attn.ttir" ||
  fail "tl.debug_barrier did not become ttg.barrier all"
if [ -n "$TRITON_OPT" ] && [ -x "$TRITON_OPT" ]; then
  "$TRITON_OPT" "$TMP/attn.ttir" -o /dev/null ||
    fail "triton-opt rejected the attention TTIR"
  "$TRITON_OPT" "$TMP/attn.ttir" --canonicalize -o "$TMP/ours.canon.ttir"
  "$TRITON_OPT" "$GOLDEN" --canonicalize -o "$TMP/golden.canon.ttir"
  diff -u "$TMP/golden.canon.ttir" "$TMP/ours.canon.ttir" > "$TMP/canon.diff" ||
    { cat "$TMP/canon.diff" >&2; fail "canonical TTIR differs from the golden"; }
  echo "PASS 1/5: the attention TTIR canonicalises to the golden"
else
  echo "SKIP 1b/5: triton-opt not found (set TRITON_OPT to enable)"
  echo "PASS 1/5: the attention TTIR has the expected shape"
fi

# --- 2. the ISA surface did not move ------------------------------------
cat > "$TMP/f32.s" <<'ASM'
fdiv r16, r17, r18
exp2 r16, r17
ASM
"$MC" -triple=gpumin -show-encoding "$TMP/f32.s" |
  sed -n 's/.*encoding: \[\(.*\)\]/\1/p' > "$TMP/bytes.txt"
grep -q '^0x03,0x90,0x11,0x1e$' "$TMP/bytes.txt" ||
  fail "fdiv no longer encodes as FALU funct 3"
grep -q '^0x00,0x00,0x11,0x2e$' "$TMP/bytes.txt" ||
  fail "exp2 no longer encodes as SFU opcode 0x0B funct 0"
"$MC" -triple=gpumin -disassemble "$TMP/bytes.txt" > "$TMP/round.s"
grep -q 'fdiv r16, r17, r18' "$TMP/round.s" || fail "fdiv did not round-trip"
grep -q 'exp2 r16, r17' "$TMP/round.s" || fail "exp2 did not round-trip"
echo "PASS 2/5: the capstone adds no instruction; fdiv/exp2 still round-trip"

# --- 3. codegen and the barrier ordering --------------------------------
"$TOOL" --emit-llvm "${EMIT[@]}" "$SRC" -o "$TMP/attn.ll"
"$LLC" -march=gpumin -O1 -verify-machineinstrs "$TMP/attn.ll" \
  -o "$TMP/attn.s" || fail "llc rejected the attention kernel"
barriers=$(grep -c 'bar.sync' "$TMP/attn.s")
[ "$barriers" -ge 5 ] || fail "expected 4 reduction barriers + 1 debug barrier, got $barriers"
last_bar=$(grep -n 'bar.sync' "$TMP/attn.s" | tail -1 | cut -d: -f1)
st_before=$(awk -v b="$last_bar" 'NR < b && /st\.global/ {n++} END {print n+0}' "$TMP/attn.s")
ld_after=$(awk -v b="$last_bar" 'NR > b && /ld\.global/ {n++} END {print n+0}' "$TMP/attn.s")
[ "$st_before" -ge 1 ] || fail "no P store before the debug barrier"
[ "$ld_after" -ge 1 ] || fail "no P reload after the debug barrier"
grep -qE '^[[:space:]]+exp2 ' "$TMP/attn.s" || fail "no exp2 in the assembly"
grep -qE '^[[:space:]]+fdiv ' "$TMP/attn.s" || fail "no fdiv in the assembly"
echo "PASS 3/5: P stores land before the debug barrier and reloads after it"

# --- 4. simulator semantics ---------------------------------------------
python3 "$HERE/triton/e2e/run_e2e.py" "$E2E" --bin "$BIN" --sim "$SIM" ||
  fail "the 8x8 attention output mismatched the f32 reference"
echo "PASS 4/5: the 64 attention outputs match the f32 reference bit-exactly"

# --- 5. actionable diagnostics ------------------------------------------
cat > "$TMP/badbar.py" <<'PY'
import triton
import triton.language as tl

@triton.jit
def k(p):
    tl.debug_barrier(1)
    tl.store(p, 0)
PY
if "$TOOL" --emit-ttir --arg-type p=ptr "$TMP/badbar.py" \
      > /dev/null 2> "$TMP/badbar.err"; then
  fail "tl.debug_barrier with an argument was accepted"
fi
grep -q 'tl.debug_barrier takes no arguments' "$TMP/badbar.err" ||
  { cat "$TMP/badbar.err" >&2; fail "the barrier diagnostic is not actionable"; }

cat > "$TMP/colslice.py" <<'PY'
import triton
import triton.language as tl

@triton.jit
def k(p):
    r = tl.arange(0, 8)[:, None]
    c = tl.arange(0, 8)[None, :]
    x = tl.load(p + r * 8 + c)
    y = x[:, 0]
    tl.store(p, y)
PY
if "$TOOL" --emit-ttir --arg-type p=ptr "$TMP/colslice.py" \
      > /dev/null 2> "$TMP/colslice.err"; then
  fail "a computed-tile column slice was accepted"
fi
grep -q 'only supports \[:, None\] and \[None, :\]' "$TMP/colslice.err" ||
  { cat "$TMP/colslice.err" >&2; fail "the slice diagnostic is not actionable"; }
echo "PASS 5/5: the barrier and computed-slice diagnostics are clear"

echo
echo "Chapter 32: 5/5 checks passed"
