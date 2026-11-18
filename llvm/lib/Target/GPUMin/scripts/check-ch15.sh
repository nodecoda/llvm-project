#!/usr/bin/env bash
#
# Chapter 15 gate: SIMT acceptance across the three sample kernels.
#
# Usage: check-ch15.sh <llvm-bin-dir>
#
# What each group proves:
#   1. vecadd: 2 blocks x 8 threads through the register ABI, plus its stats;
#   2. saxpy:  a shift-add kernel through the launch ABI;
#   3. reduce: per-lane strided sums with lane-varying trip counts;
#   4. the statistics are real measurements: they scale with the grid, and
#      lane_instrs / issued separates uniform from divergent programs.
#
set -euo pipefail

BIN=${1:?usage: check-ch15.sh <llvm-bin-dir>}
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

llc() {
  "$BIN/llc" -march=gpumin -O1 -verify-machineinstrs "$@"
}

sim() {
  python3 "$SIM" "$@"
}

stats() { # stats <key> <logfile>: pull one number out of the statistics line
  grep -o "$1=[0-9]*" "$2" | head -1 | cut -d= -f2
}

expect_words() { # expect_words <file> <python expression over t and g>
  python3 - "$1" "$2" <<'PY'
import sys
got = [int(x) for x in open(sys.argv[1]).read().split()]
want = eval(sys.argv[2])
if got != want:
    print("got  %s" % got, file=sys.stderr)
    print("want %s" % want, file=sys.stderr)
    sys.exit(1)
PY
}

# --- 1. vecadd -----------------------------------------------------------
llc -filetype=obj "$E2E/vecadd.ll" -o "$TMP/vecadd.o"
sim "$TMP/vecadd.o" --kernel vecadd --blocks 2 --threads 8 \
  --arg 0x0 --arg 0x2000 --arg 0x4000 --arg 8 \
  --fill 0x0=0,1,8 --fill 0x2000=0,3,8 \
  --dump 0x4000=8 --stats > "$TMP/vecadd.txt" 2> "$TMP/vecadd.log"
expect_words "$TMP/vecadd.txt" "[4 * t for t in range(8)]" ||
  fail "vecadd values"
[ "$(stats stores "$TMP/vecadd.log")" = "16" ] ||
  fail "vecadd stores: $(stats stores "$TMP/vecadd.log")"
[ "$(stats loads "$TMP/vecadd.log")" = "32" ] ||
  fail "vecadd loads: $(stats loads "$TMP/vecadd.log")"
echo "PASS 1/4: 2 blocks x 8 threads add correctly and report 32 loads/16 stores"

# --- 2. saxpy ------------------------------------------------------------
llc -filetype=obj "$E2E/saxpy.ll" -o "$TMP/saxpy.o"
sim "$TMP/saxpy.o" --kernel saxpy_kernel --launch "$E2E/saxpy.json" --stats \
  > "$TMP/saxpy.txt" 2> "$TMP/saxpy.log"
expect_words "$TMP/saxpy.txt" "[13 + 5 * g for g in range(16)]" ||
  fail "saxpy values"
[ "$(stats stores "$TMP/saxpy.log")" = "16" ] ||
  fail "saxpy stores: $(stats stores "$TMP/saxpy.log")"
# 4 argument-block loads + 2 data loads for each of the 16 lanes.
[ "$(stats loads "$TMP/saxpy.log")" = "96" ] ||
  fail "saxpy loads: $(stats loads "$TMP/saxpy.log")"
llc "$E2E/saxpy.ll" -o "$TMP/saxpy.s"
grep -q '\bshl\b' "$TMP/saxpy.s" || fail "saxpy did not use the shift-add"
echo "PASS 2/4: saxpy computes 3x + y through the launch descriptor ABI"

# --- 3. reduce -----------------------------------------------------------
llc -filetype=obj "$E2E/reduce.ll" -o "$TMP/reduce.o"
sim "$TMP/reduce.o" --kernel reduce_kernel --launch "$E2E/reduce.json" --stats \
  > "$TMP/reduce.txt" 2> "$TMP/reduce.log"
expect_words "$TMP/reduce.txt" \
  "[sum(t + 8 * k for k in range(8) if t + 8 * k < 60) for t in range(8)]" ||
  fail "reduce values"
[ "$(stats splits "$TMP/reduce.log")" -ge 1 ] ||
  fail "reduce never diverged"
[ "$(stats frames_max "$TMP/reduce.log")" = "2" ] ||
  fail "reduce frames_max: $(stats frames_max "$TMP/reduce.log")"
echo "PASS 3/4: per-lane strided sums diverge and reconverge"

# --- 4. the statistics are measurements ---------------------------------
cat > "$TMP/half.json" <<'JSON'
{
  "grid": 1,
  "block": 8,
  "args_addr": 256,
  "args": [0, 4096, 8192, 8],
  "fill": [
    { "addr": 0, "start": 1, "step": 1, "count": 8 },
    { "addr": 4096, "start": 10, "step": 2, "count": 8 }
  ],
  "dump": [ { "addr": 8192, "count": 8 } ]
}
JSON
sim "$TMP/saxpy.o" --kernel saxpy_kernel --launch "$TMP/half.json" --stats \
  > "$TMP/half.txt" 2> "$TMP/half.log"
half=$(stats issued "$TMP/half.log")
full=$(stats issued "$TMP/saxpy.log")
[ "$((half * 2))" = "$full" ] ||
  fail "issued did not scale with the grid: $half x 2 != $full"

# A uniform warp executes every instruction with all 8 lanes; a divergent one
# spends part of its time on subsets.
issued=$(stats issued "$TMP/vecadd.log")
lanes=$(stats lane_instrs "$TMP/vecadd.log")
[ "$((issued * 8))" = "$lanes" ] ||
  fail "uniform vecadd: $issued issues but $lanes lane-instructions"
issued=$(stats issued "$TMP/reduce.log")
lanes=$(stats lane_instrs "$TMP/reduce.log")
[ "$lanes" -lt "$((issued * 8))" ] ||
  fail "divergent reduce: $issued issues, $lanes lane-instructions (no divergence?)"
echo "PASS 4/4: statistics scale with the grid and expose divergence"

echo "OK: chapter 15 gate passed"
