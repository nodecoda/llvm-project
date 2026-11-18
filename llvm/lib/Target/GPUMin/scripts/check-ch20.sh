#!/usr/bin/env bash
#
# Chapter 20 gate: 4x4 matmul, naive against tiled.
#
# Usage: check-ch20.sh <llvm-bin-dir>
#
# What each group proves:
#   1. both kernels compute the reference product;
#   2. tiling cuts global traffic fourfold and moves it into the shared window;
#   3. with latency off, tiling is *slower* -- it costs instructions;
#   4. with a realistic latency split, tiling is faster, which is the only
#      reason the shared window and the barrier exist.
#
set -euo pipefail

BIN=${1:?usage: check-ch20.sh <llvm-bin-dir>}
HERE=$(cd "$(dirname "$0")" && pwd)
SIM="$HERE/../tools/gpumin_sim.py"
E2E="$HERE/e2e"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
export PYTHONDONTWRITEBYTECODE=1

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

stats() { # stats <key> <logfile>
  grep -o "$1=[0-9]*" "$2" | head -1 | cut -d= -f2
}

"$BIN/llc" -march=gpumin -O1 -verify-machineinstrs -filetype=obj \
  "$E2E/matmul.ll" -o "$TMP/matmul.o" || fail "llc rejected matmul.ll"

run() { # run <kernel> <outfile> <logfile> [extra args...]
  local kern=$1 out=$2 log=$3
  shift 3
  python3 "$SIM" "$TMP/matmul.o" --kernel "$kern" --blocks 1 --threads 16 \
    --arg 0x0 --arg 0x800 --arg 0x1000 \
    --fill 0x0=1,1,16 --fill 0x800=17,1,16 \
    --dump 0x1000=16 --stats "$@" > "$out" 2> "$log"
}

reference() {
  python3 - <<'PY'
A = [[1 + 4 * i + j for j in range(4)] for i in range(4)]
B = [[17 + 4 * i + j for j in range(4)] for i in range(4)]
print(" ".join(str(sum(A[i][k] * B[k][j] for k in range(4)))
               for i in range(4) for j in range(4)))
PY
}

want=$(reference)

# --- 1. both kernels are correct ---------------------------------------
for kern in matmul_naive matmul_tiled; do
  run "$kern" "$TMP/$kern.txt" "$TMP/$kern.log" --latency 0
  got=$(cat "$TMP/$kern.txt")
  [ "$got" = "$want" ] || fail "$kern: got [$got] want [$want]"
done
grep -q 'barriers=1' "$TMP/matmul_tiled.log" ||
  fail "the tiled kernel should pass exactly one block barrier"
grep -q 'barriers=0' "$TMP/matmul_naive.log" ||
  fail "the naive kernel needs no barrier"
echo "PASS 1/4: naive and tiled agree with the reference product"

# --- 2. tiling trades global traffic for shared traffic ----------------
nglb=$(stats glb_ld "$TMP/matmul_naive.log")
tglb=$(stats glb_ld "$TMP/matmul_tiled.log")
nshm=$(stats shm_ld "$TMP/matmul_naive.log")
tshm=$(stats shm_ld "$TMP/matmul_tiled.log")
[ "$((nglb / tglb))" = "4" ] ||
  fail "global loads: naive=$nglb tiled=$tglb, want a 4x reduction"
[ "$nshm" = "0" ] || fail "naive used shared memory ($nshm loads)"
[ "$tshm" = "128" ] ||
  fail "tiled shared loads: $tshm, want 8 per lane x 16 lanes"
[ "$(stats glb_st "$TMP/matmul_tiled.log")" = "16" ] ||
  fail "each lane writes its own output element"
echo "PASS 2/4: global loads $nglb -> $tglb, shared loads $nshm -> $tshm"

# --- 3. without latency the tiling overhead shows ----------------------
run matmul_naive "$TMP/n0.txt" "$TMP/n0.log" --latency 0
run matmul_tiled "$TMP/t0.txt" "$TMP/t0.log" --latency 0
nc0=$(stats cycles "$TMP/n0.log")
tc0=$(stats cycles "$TMP/t0.log")
[ "$tc0" -gt "$nc0" ] ||
  fail "latency 0: tiled ($tc0) should cost more than naive ($nc0)"
echo "PASS 3/4: without memory latency, tiling is slower ($tc0 vs $nc0 cycles)"

# --- 4. with a realistic latency split, tiling pays --------------------
run matmul_naive "$TMP/n1.txt" "$TMP/n1.log" --latency global=32,shared=2
run matmul_tiled "$TMP/t1.txt" "$TMP/t1.log" --latency global=32,shared=2
nc1=$(stats cycles "$TMP/n1.log")
tc1=$(stats cycles "$TMP/t1.log")
[ "$tc1" -lt "$nc1" ] ||
  fail "latency global=32: tiled ($tc1) should beat naive ($nc1)"
[ "$(stats stall_cycles "$TMP/t1.log")" -lt \
  "$(stats stall_cycles "$TMP/n1.log")" ] ||
  fail "tiled should accumulate fewer stalled cycles"
echo "PASS 4/4: with latency 32/2, tiling wins ($tc1 vs $nc1 cycles)"

echo "OK: chapter 20 gate passed"
