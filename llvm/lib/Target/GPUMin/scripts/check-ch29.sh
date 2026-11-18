#!/usr/bin/env bash
#
# Chapter 29 gate: 2D tensors, 2D grids and layout conversion.
#
# Usage: check-ch29.sh <llvm-bin-dir>
#
# What this proves:
#   1. [:, None] / [None, :] lower to tt.expand_dims, mixed row/column tensors
#      to tt.broadcast, .T to tt.trans [1,0], and program_id(1) to
#      tt.get_program_id y; the canonical form matches the golden;
#   2. sr.blockidx / sr.blockidy encode and decode round-trip;
#   3. llc selects the 2D grid registers and the integer multiply used by
#      index arithmetic, and the transpose reaches shared memory plus two
#      barriers, with -verify-machineinstrs;
#   4. the simulator reproduces copy, transpose, broadcast and a 2x2 grid
#      over a 16x16 matrix;
#   5. unsupported subscripts, 1D transposes, oversized tiles and 2D
#      reductions are rejected with actionable diagnostics.
#
# TRITON_OPT may point at triton-opt; check 1b is SKIP when it is absent.
#
set -euo pipefail

BIN=${1:?usage: check-ch29.sh <llvm-bin-dir>}
HERE=$(cd "$(dirname "$0")" && pwd)
TOOL="$BIN/gpumin-triton"
LLC="$BIN/llc"
MC="$BIN/llvm-mc"
SIM="$HERE/../tools/gpumin_sim.py"
SRC="$HERE/triton/layout2d.py"
GOLDEN="$HERE/triton/layout2d.ttir"
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
EMIT=(--arg-type in_ptr=ptr --arg-type out_ptr=ptr)

# --- 1. TTIR shape and canonical golden ---------------------------------
"$TOOL" --emit-ttir "${EMIT[@]}" "$SRC" -o "$TMP/l2.ttir"
grep -q 'tt.expand_dims .* {axis = 1 : i32} : tensor<8xi32> -> tensor<8x1xi32>' \
  "$TMP/l2.ttir" || fail "[:, None] did not become expand_dims axis 1"
grep -q 'tt.expand_dims .* {axis = 0 : i32} : tensor<8xi32> -> tensor<1x8xi32>' \
  "$TMP/l2.ttir" || fail "[None, :] did not become expand_dims axis 0"
grep -q 'tt.broadcast .* : tensor<8x1xi32> -> tensor<8x8xi32>' \
  "$TMP/l2.ttir" || fail "the column vector was not broadcast"
grep -q 'tt.broadcast .* : tensor<1x8xi32> -> tensor<8x8xi32>' \
  "$TMP/l2.ttir" || fail "the row vector was not broadcast"
grep -q 'tt.trans .* {order = array<i32: 1, 0>} : tensor<8x8xi32>' \
  "$TMP/l2.ttir" || fail "tl.trans did not print order [1, 0]"
grep -q 'tt.get_program_id x : i32' "$TMP/l2.ttir" ||
  fail "program_id(0) is missing"
grep -q 'tt.get_program_id y : i32' "$TMP/l2.ttir" ||
  fail "program_id(1) did not become get_program_id y"
if [ -n "$TRITON_OPT" ] && [ -x "$TRITON_OPT" ]; then
  "$TRITON_OPT" "$TMP/l2.ttir" -o /dev/null ||
    fail "triton-opt rejected the 2D TTIR"
  "$TRITON_OPT" "$TMP/l2.ttir" --canonicalize -o "$TMP/ours.canon.ttir"
  "$TRITON_OPT" "$GOLDEN" --canonicalize -o "$TMP/golden.canon.ttir"
  diff -u "$TMP/golden.canon.ttir" "$TMP/ours.canon.ttir" \
    > "$TMP/canon.diff" ||
    { cat "$TMP/canon.diff" >&2; fail "canonical TTIR differs from the golden"; }
  grep -q 'dense<8>' "$TMP/ours.canon.ttir" ||
    fail "r * 8 did not fold to a dense constant"
  echo "PASS 1/5: 2D TTIR canonicalises to the golden"
else
  echo "SKIP 1b/5: triton-opt not found (set TRITON_OPT to enable)"
  echo "PASS 1/5: 2D TTIR has the expected shape"
fi

# --- 2. MC round-trip ----------------------------------------------------
cat > "$TMP/grid.s" <<'ASM'
sr.blockidx r1
sr.blockidy r1
ASM
"$MC" -triple=gpumin -show-encoding "$TMP/grid.s" |
  sed -n 's/.*encoding: \[\(.*\)\]/\1/p' > "$TMP/bytes.txt"
[ "$(wc -l < "$TMP/bytes.txt")" -eq 2 ] || fail "expected two encodings"
"$MC" -triple=gpumin -disassemble "$TMP/bytes.txt" > "$TMP/round.s"
grep -qE '^[[:space:]]*sr\.blockidx r1' "$TMP/round.s" ||
  fail "sr.blockidx did not round-trip"
grep -qE '^[[:space:]]*sr\.blockidy r1' "$TMP/round.s" ||
  fail "sr.blockidy did not round-trip"
echo "PASS 2/5: sr.blockidx/sr.blockidy encode and decode"

# --- 3. codegen ----------------------------------------------------------
"$TOOL" --emit-llvm "${EMIT[@]}" "$SRC" -o "$TMP/l2.ll"
"$LLC" -march=gpumin -O1 -verify-machineinstrs "$TMP/l2.ll" -o "$TMP/l2.s" ||
  fail "llc rejected the 2D kernels"
grep -q 'sr.blockidx' "$TMP/l2.s" || fail "no sr.blockidx in the assembly"
grep -q 'sr.blockidy' "$TMP/l2.s" || fail "no sr.blockidy in the assembly"
grep -qE '^[[:space:]]+mul ' "$TMP/l2.s" ||
  fail "index arithmetic did not select integer mul"
grep -q 'st.shared' "$TMP/l2.s" || fail "transpose did not write shared memory"
grep -q 'ld.shared' "$TMP/l2.s" || fail "transpose did not read shared memory"
[ "$(grep -c 'bar.sync' "$TMP/l2.s")" -ge 2 ] ||
  fail "transpose did not use two barriers"
echo "PASS 3/5: grid registers, integer mul, shared transpose and barriers"

# --- 4. simulator semantics ---------------------------------------------
"$LLC" -march=gpumin -O1 -verify-machineinstrs -filetype=obj \
  "$TMP/l2.ll" -o "$TMP/l2.o" || fail "llc rejected the 2D object"
cat > "$TMP/copy.json" <<'JSON'
{
  "grid": 1,
  "block": 32,
  "args_addr": 4096,
  "args": [0, 8192],
  "fill": [ { "addr": 0, "start": 100, "step": 1, "count": 64 } ],
  "dump": [ { "addr": 8192, "count": 64 } ]
}
JSON
want=$(python3 - <<'PY'
print(" ".join(str(100 + i) for i in range(64)))
PY
)
got=$(python3 "$SIM" "$TMP/l2.o" --kernel copy2d_kernel --launch "$TMP/copy.json") ||
  fail "the simulator rejected copy2d_kernel"
[ "$got" = "$want" ] || fail "8x8 copy mismatch"

cat > "$TMP/trans.json" <<'JSON'
{
  "grid": 1,
  "block": 32,
  "args_addr": 4096,
  "args": [0, 8192],
  "fill": [ { "addr": 0, "start": 0, "step": 1, "count": 64 } ],
  "dump": [ { "addr": 8192, "count": 64 } ]
}
JSON
want=$(python3 - <<'PY'
print(" ".join(str(c * 8 + r) for r in range(8) for c in range(8)))
PY
)
got=$(python3 "$SIM" "$TMP/l2.o" --kernel trans2d_kernel --launch "$TMP/trans.json") ||
  fail "the simulator rejected trans2d_kernel"
[ "$got" = "$want" ] || fail "8x8 transpose mismatch"

cat > "$TMP/bcast.json" <<'JSON'
{
  "grid": 1,
  "block": 32,
  "args_addr": 4096,
  "args": [8192],
  "dump": [ { "addr": 8192, "count": 64 } ]
}
JSON
want=$(python3 - <<'PY'
print(" ".join(str(r + 10 * c) for r in range(8) for c in range(8)))
PY
)
got=$(python3 "$SIM" "$TMP/l2.o" --kernel bcast2d_kernel --launch "$TMP/bcast.json") ||
  fail "the simulator rejected bcast2d_kernel"
[ "$got" = "$want" ] || fail "row/column broadcast mismatch"

cat > "$TMP/grid2d.json" <<'JSON'
{
  "grid_dims": 2,
  "grid": [2, 2],
  "block": 32,
  "args_addr": 4096,
  "args": [0, 8192],
  "fill": [ { "addr": 0, "start": 0, "step": 1, "count": 256 } ],
  "dump": [ { "addr": 8704, "count": 32 } ]
}
JSON
want=$(python3 - <<'PY'
# The dump starts at element 128 (row 8): the by=1 half of the 16x16
# matrix.  Row 8 also receives the bx=1 block, whose values are biased by
# one more than the bx=0 block.
vals = []
for r in (8, 9):
    for bx in (0, 1):
        for c in range(8):
            vals.append(r * 16 + bx * 8 + c + bx + 1)
print(" ".join(str(v) for v in vals))
PY
)
got=$(python3 "$SIM" "$TMP/l2.o" --kernel grid2d_kernel --launch "$TMP/grid2d.json") ||
  fail "the simulator rejected grid2d_kernel"
[ "$got" = "$want" ] || fail "2D grid addressing mismatch: got $got want $want"
echo "PASS 4/5: copy, transpose, broadcast and the 2x2 grid match references"

# --- 5. actionable diagnostics ------------------------------------------
cat > "$TMP/badsub.py" <<'PY'
import triton
import triton.language as tl

@triton.jit
def k(p):
    a = tl.arange(0, 32)
    v = a[0]
    tl.store(p, v)
PY
if "$TOOL" --emit-ttir --arg-type p=ptr "$TMP/badsub.py" \
      > /dev/null 2> "$TMP/badsub.err"; then
  fail "a scalar subscript was accepted"
fi
grep -q 'only supports \[:, None\] and \[None, :\]' "$TMP/badsub.err" ||
  { cat "$TMP/badsub.err" >&2; fail "subscript diagnostic is not actionable"; }

cat > "$TMP/badtrans.py" <<'PY'
import triton
import triton.language as tl

@triton.jit
def k(p):
    a = tl.arange(0, 32)
    v = tl.trans(a)
    tl.store(p, v)
PY
if "$TOOL" --emit-ttir --arg-type p=ptr "$TMP/badtrans.py" \
      > /dev/null 2> "$TMP/badtrans.err"; then
  fail "a 1D transpose was accepted"
fi
grep -q 'tl.trans requires a 2D tensor' "$TMP/badtrans.err" ||
  { cat "$TMP/badtrans.err" >&2; fail "transpose diagnostic is not actionable"; }

cat > "$TMP/bigtile.py" <<'PY'
import triton
import triton.language as tl

@triton.jit
def k(p):
    r = tl.arange(0, 32)[:, None]
    c = tl.arange(0, 32)[None, :]
    v = r + c
    tl.store(p + r * 32 + c, v)
PY
if "$TOOL" --emit-llvm --arg-type p=ptr "$TMP/bigtile.py" \
      > /dev/null 2> "$TMP/bigtile.err"; then
  fail "a 32x32 tile was accepted"
fi
grep -q 'at most 8 elements per thread' "$TMP/bigtile.err" ||
  { cat "$TMP/bigtile.err" >&2; fail "tile-size diagnostic is not actionable"; }

cat > "$TMP/badreduce.py" <<'PY'
import triton
import triton.language as tl

@triton.jit
def k(p):
    r = tl.arange(0, 8)[:, None]
    c = tl.arange(0, 8)[None, :]
    x = tl.load(p + r * 8 + c)
    v = tl.max(x, axis=0)
    tl.store(p, v)
PY
if "$TOOL" --emit-ttir --arg-type p=ptr "$TMP/badreduce.py" \
      > /dev/null 2> "$TMP/badreduce.err"; then
  fail "a 2D reduction was accepted"
fi
grep -q 'keep_dims=True' "$TMP/badreduce.err" ||
  { cat "$TMP/badreduce.err" >&2; fail "2D-reduction diagnostic is not actionable"; }
echo "PASS 5/5: bad subscripts, 1D transposes, big tiles and 2D reductions fail"

echo
echo "Chapter 29: 5/5 checks passed"
