#!/usr/bin/env bash
#
# Chapter 18 gate: the issue-slot scheduling model.
#
# Usage: check-ch18.sh <llvm-bin-dir>
#
# What each group proves:
#   1. every retired instruction is classified into exactly one unit, and the
#      LSU count matches the memory traffic;
#   2. one warp issues at most one instruction per cycle, so cycles == issued;
#   3. four warps overlap units, but lockstep contention keeps them above the
#      single-unit bound;
#   4. widening a unit only helps when that unit is the bottleneck.
#
set -euo pipefail

BIN=${1:?usage: check-ch18.sh <llvm-bin-dir>}
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

stats() { # stats <key> <logfile>
  grep -o "$1=[0-9]*" "$2" | head -1 | cut -d= -f2
}

llc -filetype=obj "$E2E/vecadd.ll" -o "$TMP/vecadd.o"
llc -filetype=obj "$E2E/barrier.ll" -o "$TMP/barrier.o"

run() { # run <object> <kernel> <threads> <outfile> <logfile> [extra args...]
  local obj=$1 kern=$2 threads=$3 out=$4 log=$5
  shift 5
  python3 "$SIM" "$obj" --kernel "$kern" --blocks 1 --threads "$threads" \
    --arg 0x0 --arg 0x2000 --arg 0x4000 --arg "$threads" \
    --fill 0x0=0,1,"$threads" --fill 0x2000=0,3,"$threads" \
    --dump 0x4000=1 --stats "$@" > "$out" 2> "$log"
}

# --- 1. every instruction lands in exactly one unit ----------------------
run "$TMP/vecadd.o" vecadd 8 "$TMP/v8.txt" "$TMP/v8.log"
issued=$(stats issued "$TMP/v8.log")
alu=$(stats alu "$TMP/v8.log")
lsu=$(stats lsu "$TMP/v8.log")
bar=$(stats bar "$TMP/v8.log")
[ "$((alu + lsu + bar))" = "$issued" ] ||
  fail "vecadd: alu=$alu lsu=$lsu bar=$bar do not add up to issued=$issued"
[ "$((lsu * 8))" = "$(( $(stats loads "$TMP/v8.log") +
                        $(stats stores "$TMP/v8.log") ))" ] ||
  fail "vecadd: lsu=$lsu does not match the 8-lane memory traffic"
[ "$bar" = "0" ] || fail "vecadd has no barrier but reports bar=$bar"

python3 "$SIM" "$TMP/barrier.o" --kernel rotate --blocks 1 --threads 32 \
  --arg 0x0 --arg 0x2000 --fill 0x0=100,1,32 --dump 0x2000=1 --stats \
  > "$TMP/r.txt" 2> "$TMP/r.log"
issued=$(stats issued "$TMP/r.log")
[ "$(( $(stats alu "$TMP/r.log") + $(stats lsu "$TMP/r.log") +
       $(stats bar "$TMP/r.log") ))" = "$issued" ] ||
  fail "rotate: the three unit counts do not add up to issued"
[ "$(stats bar "$TMP/r.log")" = "4" ] ||
  fail "rotate: one barrier per warp, got bar=$(stats bar "$TMP/r.log")"
echo "PASS 1/4: every instruction retires on exactly one unit"

# --- 2. a warp issues at most one instruction per cycle -----------------
cycles=$(stats cycles "$TMP/v8.log")
[ "$cycles" = "$(stats issued "$TMP/v8.log")" ] ||
  fail "one warp: cycles=$cycles but issued=$(stats issued "$TMP/v8.log")"
python3 "$SIM" "$TMP/barrier.o" --kernel rotate --blocks 1 --threads 8 \
  --arg 0x0 --arg 0x2000 --fill 0x0=100,1,8 --dump 0x2000=1 --stats \
  > "$TMP/r1.txt" 2> "$TMP/r1.log"
[ "$(stats cycles "$TMP/r1.log")" = "$(stats issued "$TMP/r1.log")" ] ||
  fail "rotate with one warp: cycles != issued"
echo "PASS 2/4: one warp takes one cycle per instruction"

# --- 3. four warps overlap units but contend in lockstep ----------------
run "$TMP/vecadd.o" vecadd 32 "$TMP/v32.txt" "$TMP/v32.log"
c4=$(stats cycles "$TMP/v32.log")
a4=$(stats alu "$TMP/v32.log")
l4=$(stats lsu "$TMP/v32.log")
b4=$(stats bar "$TMP/v32.log")
i4=$(stats issued "$TMP/v32.log")
bound=$a4
[ "$l4" -le "$bound" ] || bound=$l4
[ "$b4" -le "$bound" ] || bound=$b4
[ "$c4" -ge "$bound" ] ||
  fail "4 warps: cycles=$c4 is below the unit bound $bound"
[ "$c4" -lt "$i4" ] ||
  fail "4 warps: cycles=$c4 never overlapped anything (issued=$i4)"
[ "$c4" -lt "$((cycles * 4))" ] ||
  fail "4 warps: cycles=$c4 did not amortise the single-warp $cycles"
echo "PASS 3/4: four warps overlap units (cycles=$c4 < issued=$i4) but stay above the $bound bound"

# --- 4. widening a unit helps only if it is the bottleneck --------------
base=$(stats cycles "$TMP/r.log")
python3 "$SIM" "$TMP/barrier.o" --kernel rotate --blocks 1 --threads 32 \
  --arg 0x0 --arg 0x2000 --fill 0x0=100,1,32 --dump 0x2000=1 --stats \
  --unit-width alu=2 > "$TMP/alu2.txt" 2> "$TMP/alu2.log"
[ "$(stats cycles "$TMP/alu2.log")" -lt "$base" ] ||
  fail "alu=2 did not shorten a $base-cycle ALU-bound kernel"

python3 "$SIM" "$TMP/barrier.o" --kernel rotate --blocks 1 --threads 32 \
  --arg 0x0 --arg 0x2000 --fill 0x0=100,1,32 --dump 0x2000=1 --stats \
  --unit-width bar=4 > "$TMP/bar4.txt" 2> "$TMP/bar4.log"
[ "$(stats cycles "$TMP/bar4.log")" = "$base" ] ||
  fail "bar=4 changed a kernel whose barrier is not the bottleneck"
echo "PASS 4/4: widening the bottleneck helps, widening an idle unit does not"

echo "OK: chapter 18 gate passed"
