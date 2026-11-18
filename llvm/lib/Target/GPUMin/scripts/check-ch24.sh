#!/usr/bin/env bash
#
# Chapter 24 gate: fixed layout, TTIR -> LLVM IR and the sr.args ABI.
#
# Usage: check-ch24.sh <llvm-bin-dir>
#
# What this proves:
#   1. TTIR lowers to LLVM IR with the sr.args kernel ABI (no formal args);
#   2. tensor<256xi32> becomes 8 scalar elements per thread (r*32+tid);
#   3. the 32-thread block compiles and runs the masked i32 vecadd on the sim;
#   4. the kernel object reads sr.args/sr.tid/sr.blockid and ends in exit;
#   5. tensor sizes that are not a multiple of 32 are rejected up front.
#
set -euo pipefail

BIN=${1:?usage: check-ch24.sh <llvm-bin-dir>}
HERE=$(cd "$(dirname "$0")" && pwd)
TOOL="$BIN/gpumin-triton"
LLC="$BIN/llc"
SIM="$HERE/../tools/gpumin_sim.py"
ADD="$HERE/triton/add_i32.py"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
export PYTHONDONTWRITEBYTECODE=1

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

[ -x "$TOOL" ] || fail "missing $TOOL"
[ -x "$LLC" ] || fail "missing $LLC"

EMIT=(--emit-llvm --constexpr BLOCK=256
      --arg-type x_ptr=ptr --arg-type y_ptr=ptr --arg-type out_ptr=ptr)

# --- 1. LLVM IR shape ----------------------------------------------------
"$TOOL" "${EMIT[@]}" "$ADD" -o "$TMP/add.ll"
grep -q 'target triple = "gpumin-unknown-elf"' "$TMP/add.ll" ||
  fail "missing gpumin triple"
grep -q 'define void @add_kernel() #0 {' "$TMP/add.ll" ||
  fail "kernel still has formal parameters"
grep -q 'attributes #0 = { "gpumin_kernel" }' "$TMP/add.ll" ||
  fail "missing gpumin_kernel attribute"
grep -q '@llvm.gpumin.args()' "$TMP/add.ll" ||
  fail "missing llvm.gpumin.args"
grep -q '@llvm.gpumin.tid()' "$TMP/add.ll" ||
  fail "missing llvm.gpumin.tid"
grep -q '@llvm.gpumin.blockid()' "$TMP/add.ll" ||
  fail "missing llvm.gpumin.blockid"
if grep -q 'nvvm\|ptx' "$TMP/add.ll"; then
  fail "LLVM IR leaked NVIDIA-specific constructs"
fi
echo "PASS 1/5: TTIR lowers to a gpumin kernel with the sr.args ABI"

# --- 2. scalarisation: 256 elements / 32 threads = 8 per thread ----------
[ "$(grep -c 'store i32' "$TMP/add.ll")" -eq 8 ] ||
  fail "expected 8 scalarised stores, got $(grep -c 'store i32' "$TMP/add.ll")"
[ "$(grep -c 'load i32' "$TMP/add.ll")" -eq 20 ] ||
  fail "expected 20 loads (4 args + 8 x + 8 y)"
echo "PASS 2/5: tensor<256xi32> is scalarised to 8 elements per thread"

# --- 3. compile and run the masked vecadd --------------------------------
"$LLC" -march=gpumin -O1 -verify-machineinstrs "$TMP/add.ll" \
  -o "$TMP/add.s" || fail "llc rejected the generated IR"
"$LLC" -march=gpumin -O1 -filetype=obj "$TMP/add.ll" \
  -o "$TMP/add.o" || fail "llc failed to produce an object"

cat > "$TMP/launch.json" <<'JSON'
{
  "grid": 1,
  "block": 32,
  "args_addr": 4096,
  "args": [0, 8192, 16384, 256],
  "fill": [
    { "addr": 0, "start": 0, "step": 1, "count": 256 },
    { "addr": 8192, "start": 1000, "step": 1, "count": 256 }
  ],
  "dump": [ { "addr": 16384, "count": 8 } ]
}
JSON
got=$(python3 "$SIM" "$TMP/add.o" --kernel add_kernel --launch "$TMP/launch.json") ||
  fail "the simulator rejected the vecadd kernel"
want="1000 1002 1004 1006 1008 1010 1012 1014"
[ "$got" = "$want" ] ||
  fail "vecadd result mismatch: got '$got', want '$want'"
echo "PASS 3/5: masked i32 vecadd runs on the 32-thread block"

# --- 4. the object uses the launch ABI -----------------------------------
grep -q 'sr.args' "$TMP/add.s" || fail "assembly does not read sr.args"
grep -q 'sr.tid' "$TMP/add.s" || fail "assembly does not read sr.tid"
grep -q 'sr.blockid' "$TMP/add.s" || fail "assembly does not read sr.blockid"
grep -q $'^\texit$' "$TMP/add.s" || fail "kernel does not end in exit"
echo "PASS 4/5: the object reads sr.args/tid/blockid and exits"

# --- 5. layout errors are diagnosed --------------------------------------
if "$TOOL" --emit-llvm --constexpr BLOCK=100 \
      --arg-type x_ptr=ptr --arg-type y_ptr=ptr --arg-type out_ptr=ptr \
      "$ADD" > "$TMP/bad.out" 2> "$TMP/bad.err"; then
  fail "BLOCK=100 was accepted"
fi
grep -q 'multiple of 32' "$TMP/bad.err" ||
  { cat "$TMP/bad.err" >&2; fail "layout error is not actionable"; }
echo "PASS 5/5: non-multiple-of-32 tensors are rejected"

# A note for the tutorial: BLOCK=1024 (32 elements per thread) currently
# exceeds the backend's predicate-spill support and crashes in regalloc.
# The gate stays at the largest size the existing backend handles.
