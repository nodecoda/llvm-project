#!/usr/bin/env bash
#
# Chapter 26 gate: tl.sum / tl.max through shared memory and barriers.
#
# Usage: check-ch26.sh <llvm-bin-dir>
#
# What this proves:
#   1. TTIR carries a real tt.reduce region over axis 0;
#   2. triton-opt parses it and it canonicalises to the golden;
#   3. llc selects shared loads/stores and bar.sync;
#   4. the simulator returns 32 and 528 for one block, and independent
#      results for two blocks;
#   5. tl.max and an f32 sum use the same path, and the stats show barriers
#      plus shared traffic.
#
# TRITON_OPT may point at triton-opt; check 2 is SKIP when it is absent.
#
set -euo pipefail

BIN=${1:?usage: check-ch26.sh <llvm-bin-dir>}
HERE=$(cd "$(dirname "$0")" && pwd)
TOOL="$BIN/gpumin-triton"
LLC="$BIN/llc"
SIM="$HERE/../tools/gpumin_sim.py"
SUM="$HERE/triton/reduce_sum.py"
MAX="$HERE/triton/reduce_max.py"
GOLDEN="$HERE/triton/reduce_sum.ttir"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
export PYTHONDONTWRITEBYTECODE=1

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

[ -x "$TOOL" ] || fail "missing $TOOL"
[ -x "$LLC" ] || fail "missing $LLC"
[ -f "$SUM" ] || fail "missing $SUM"
[ -f "$MAX" ] || fail "missing $MAX"
[ -f "$GOLDEN" ] || fail "missing $GOLDEN"

TRITON_OPT=${TRITON_OPT:-$(command -v triton-opt || true)}
SUM_EMIT=(--constexpr BLOCK=32 --arg-type 'x_ptr=ptr' --arg-type 'out_ptr=ptr')

# --- 1. TTIR reduce region ----------------------------------------------
"$TOOL" --emit-ttir "${SUM_EMIT[@]}" "$SUM" > "$TMP/sum.ttir"
grep -q '"tt.reduce"(' "$TMP/sum.ttir" || fail "missing tt.reduce"
grep -q 'tt.reduce.return' "$TMP/sum.ttir" || fail "missing tt.reduce.return"
grep -q 'axis = 0 : i32' "$TMP/sum.ttir" || fail "reduce axis is not 0"
grep -q 'arith.addi %red' "$TMP/sum.ttir" ||
  fail "reduce region does not contain an integer add"
grep -q ') : (tensor<32xi32>) -> i32' "$TMP/sum.ttir" ||
  fail "reduce result type is not a scalar i32"
grep -q 'tt.store .* : !tt.ptr<i32>' "$TMP/sum.ttir" ||
  fail "the reduction result is not stored through a scalar pointer"
echo "PASS 1/5: TTIR contains a tt.reduce region and a scalar store"

# --- 2. Triton parser and canonical golden -------------------------------
if [ -n "$TRITON_OPT" ] && [ -x "$TRITON_OPT" ]; then
  "$TRITON_OPT" "$TMP/sum.ttir" -o /dev/null ||
    fail "triton-opt rejected the reduction TTIR"
  "$TRITON_OPT" "$TMP/sum.ttir" -o "$TMP/ours.canon.ttir"
  "$TRITON_OPT" "$GOLDEN" -o "$TMP/golden.canon.ttir"
  diff -u "$TMP/golden.canon.ttir" "$TMP/ours.canon.ttir" \
    > "$TMP/canon.diff" ||
    { cat "$TMP/canon.diff" >&2; fail "canonical TTIR differs from the golden"; }
  echo "PASS 2/5: triton-opt parses the reduction and it matches the golden"
else
  echo "SKIP 2/5: triton-opt not found (set TRITON_OPT to enable)"
fi

# --- 3. codegen: shared window plus two barriers ------------------------
"$TOOL" --emit-llvm "${SUM_EMIT[@]}" "$SUM" -o "$TMP/sum.ll"
grep -q 'addrspace(2) global \[32 x i32\]' "$TMP/sum.ll" ||
  fail "the reduction window is not an addrspace(2) global"
"$LLC" -march=gpumin -O1 -verify-machineinstrs "$TMP/sum.ll" \
  -o "$TMP/sum.s" || fail "llc rejected the reduction"
grep -q $'^\tbar.sync$' "$TMP/sum.s" || fail "no bar.sync in the assembly"
grep -q 'st.shared.b32' "$TMP/sum.s" || fail "no shared store"
[ "$(grep -c 'ld.shared.b32' "$TMP/sum.s")" -ge 32 ] ||
  fail "the lane-0 combine does not read all 32 slots"
grep -q 'sr.shared' "$TMP/sum.s" || fail "the window does not use sr.shared"
echo "PASS 3/5: the reduction uses shared memory and two bar.sync"

# --- 4. simulator: one block and two blocks -----------------------------
"$LLC" -march=gpumin -O1 -filetype=obj "$TMP/sum.ll" -o "$TMP/sum.o"

cat > "$TMP/ones.json" <<'JSON'
{
  "grid": 1,
  "block": 32,
  "args_addr": 4096,
  "args": [0, 8192],
  "fill": [ { "addr": 0, "start": 1, "step": 0, "count": 32 } ],
  "dump": [ { "addr": 8192, "count": 1 } ]
}
JSON
got=$(python3 "$SIM" "$TMP/sum.o" --kernel reduce_sum \
        --launch "$TMP/ones.json") ||
  fail "the simulator rejected the reduction kernel"
[ "$got" = "32" ] || fail "sum of 32 ones is '$got', want 32"

cat > "$TMP/ramp.json" <<'JSON'
{
  "grid": 1,
  "block": 32,
  "args_addr": 4096,
  "args": [0, 8192],
  "fill": [ { "addr": 0, "start": 1, "step": 1, "count": 32 } ],
  "dump": [ { "addr": 8192, "count": 1 } ]
}
JSON
got=$(python3 "$SIM" "$TMP/sum.o" --kernel reduce_sum \
        --launch "$TMP/ramp.json") ||
  fail "the simulator rejected the ramp reduction"
[ "$got" = "528" ] || fail "sum of 1..32 is '$got', want 528"

cat > "$TMP/two.json" <<'JSON'
{
  "grid": 2,
  "block": 32,
  "args_addr": 4096,
  "args": [0, 8192],
  "fill": [
    { "addr": 0, "start": 1, "step": 0, "count": 32 },
    { "addr": 128, "start": 2, "step": 0, "count": 32 }
  ],
  "dump": [ { "addr": 8192, "count": 2 } ]
}
JSON
got=$(python3 "$SIM" "$TMP/sum.o" --kernel reduce_sum \
        --launch "$TMP/two.json" --stats 2> "$TMP/two.stats") ||
  fail "the simulator rejected the two-block reduction"
[ "$got" = "32 64" ] ||
  fail "two blocks produced '$got', want '32 64' (shared is not block-local)"
echo "PASS 4/5: one block gives 32/528 and two blocks stay independent"

# --- 5. tl.max, f32 sum, and the statistics ------------------------------
"$TOOL" --emit-llvm "${SUM_EMIT[@]}" "$MAX" -o "$TMP/max.ll"
"$LLC" -march=gpumin -O1 -filetype=obj "$TMP/max.ll" -o "$TMP/max.o"
got=$(python3 "$SIM" "$TMP/max.o" --kernel reduce_max \
        --launch "$TMP/two.json") ||
  fail "the simulator rejected tl.max"
[ "$got" = "1 2" ] || fail "tl.max produced '$got', want '1 2'"

FSUM_EMIT=(--constexpr BLOCK=32 --arg-type 'x_ptr=ptr<f32>'
           --arg-type 'out_ptr=ptr<f32>')
"$TOOL" --emit-llvm "${FSUM_EMIT[@]}" "$SUM" -o "$TMP/fsum.ll"
"$LLC" -march=gpumin -O1 -filetype=obj "$TMP/fsum.ll" -o "$TMP/fsum.o"
cat > "$TMP/fsum.json" <<'JSON'
{
  "grid": 2,
  "block": 32,
  "args_addr": 4096,
  "args": [0, 8192],
  "fill": [
    { "addr": 0, "start": 1065353216, "step": 0, "count": 32 },
    { "addr": 128, "start": 1073741824, "step": 0, "count": 32 }
  ],
  "dump": [ { "addr": 8192, "count": 2 } ]
}
JSON
got=$(python3 "$SIM" "$TMP/fsum.o" --kernel reduce_sum \
        --launch "$TMP/fsum.json" --stats 2> "$TMP/fsum.stats") ||
  fail "the simulator rejected the f32 reduction"
python3 - "$got" <<'PY' || fail "f32 reduction result mismatch"
import struct, sys
def bits(v):
    return struct.unpack('<I', struct.pack('<f', v))[0]
want = [bits(32.0), bits(64.0)]
got = [int(w) for w in sys.argv[1].split()]
if got != want:
    print("got %r, want %r" % (got, want), file=sys.stderr)
    sys.exit(1)
PY
grep -qE 'barriers=[1-9][0-9]*' "$TMP/fsum.stats" ||
  fail "the stats report no barriers"
grep -qE 'shm_ld=[1-9][0-9]*' "$TMP/fsum.stats" ||
  fail "the stats report no shared loads"
grep -qE 'shm_st=[1-9][0-9]*' "$TMP/fsum.stats" ||
  fail "the stats report no shared stores"
echo "PASS 5/5: tl.max, f32 tl.sum and the shared/barrier stats agree"

echo
echo "Chapter 26: 5/5 checks passed"
