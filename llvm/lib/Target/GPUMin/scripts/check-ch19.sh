#!/usr/bin/env bash
#
# Chapter 19 gate: load latency and occupancy.
#
# Usage: check-ch19.sh <llvm-bin-dir>
#
# What each group proves:
#   1. a load stalls its warp for exactly the modelled number of cycles;
#   2. the default latency is 0, so chapters 12-18 stay reproducible;
#   3. more warps hide the same stall: cost per instruction falls;
#   4. occupancy is a cap: the register budget binds only past a threshold.
#
set -euo pipefail

BIN=${1:?usage: check-ch19.sh <llvm-bin-dir>}
HERE=$(cd "$(dirname "$0")" && pwd)
TOOLS="$HERE/../tools"
SIM="$TOOLS/gpumin_sim.py"
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

run() { # run <threads> <outfile> <logfile> [extra args...]
  local threads=$1 out=$2 log=$3
  shift 3
  python3 "$SIM" "$TMP/vecadd.o" --kernel vecadd --blocks 1 \
    --threads "$threads" --arg 0x0 --arg 0x2000 --arg 0x4000 --arg "$threads" \
    --fill 0x0=0,1,"$threads" --fill 0x2000=0,3,"$threads" \
    --dump 0x4000=1 --stats "$@" > "$out" 2> "$log"
}

# --- 1. a load stalls for exactly the modelled number of cycles ---------
run 8 "$TMP/l8.txt" "$TMP/l8.log" --latency 8
issued=$(stats issued "$TMP/l8.log")
ldops=$(stats ld_ops "$TMP/l8.log")
stall=$(stats stall_cycles "$TMP/l8.log")
cycles=$(stats cycles "$TMP/l8.log")
[ "$stall" = "$((ldops * 8))" ] ||
  fail "stall_cycles=$stall, want ld_ops($ldops) * 8"
[ "$cycles" = "$((issued + stall))" ] ||
  fail "one warp: cycles=$cycles, want issued($issued) + stall($stall)"
echo "PASS 1/4: $ldops loads stall $stall cycles, and one warp cannot hide any of it"

# --- 2. the default is still the chapter 18 model -----------------------
run 8 "$TMP/d8.txt" "$TMP/d8.log"
[ "$(stats cycles "$TMP/d8.log")" = "$(stats issued "$TMP/d8.log")" ] ||
  fail "default cycles != issued; chapters 12-18 would no longer reproduce"
[ "$(stats stall_cycles "$TMP/d8.log")" = "0" ] ||
  fail "default model stalled without --latency"
echo "PASS 2/4: latency stays 0 unless asked for"

# --- 3. occupancy hides stalled cycles ---------------------------------
for n in 8 16 32; do
  run "$n" "$TMP/h$n.txt" "$TMP/h$n.log" --latency 8
  eval "i$n=\$(stats issued \"\$TMP/h$n.log\")"
  eval "c$n=\$(stats cycles \"\$TMP/h$n.log\")"
done
# cycles per instruction must fall as warps are added.
[ $((c8 * i16)) -gt $((c16 * i8)) ] ||
  fail "2 warps did not amortise latency: $c8/$i8 vs $c16/$i16"
[ $((c16 * i32)) -gt $((c32 * i16)) ] ||
  fail "4 warps did not amortise latency further: $c16/$i16 vs $c32/$i32"
stall32=$(stats stall_cycles "$TMP/h32.log")
[ "$stall32" -gt $((stall * 2)) ] ||
  fail "4 warps should accumulate far more stall cycles, got $stall32"
echo "PASS 3/4: per-instruction cost falls from $c8/$i8 to $c32/$i32 while the block absorbs $stall32 stalled cycles"

# --- 4. occupancy is a cap, not a measurement --------------------------
run 32 "$TMP/o32.txt" "$TMP/o32.log" --regs-per-thread 32
[ "$(stats warps_per_sm "$TMP/o32.log")" = "64" ] ||
  fail "32 regs/thread should hit the 64-warp cap, got $(stats warps_per_sm "$TMP/o32.log")"
run 32 "$TMP/o129.txt" "$TMP/o129.log" --regs-per-thread 129
[ "$(stats warps_per_sm "$TMP/o129.log")" = "63" ] ||
  fail "129 regs/thread should be register-bound (63), got $(stats warps_per_sm "$TMP/o129.log")"
run 32 "$TMP/o255.txt" "$TMP/o255.log" --regs-per-thread 255
[ "$(stats warps_per_sm "$TMP/o255.log")" = "32" ] ||
  fail "255 regs/thread should give 32 warps, got $(stats warps_per_sm "$TMP/o255.log")"

# The kernel's real footprint, counted from its own assembly.
llc "$E2E/vecadd.ll" -o "$TMP/vecadd.s"
used=$(grep -oE '\br[0-9]+\b' "$TMP/vecadd.s" | sed 's/^r//' | sort -n |
       tail -1)
[ -n "$used" ] || fail "no registers found in the assembly"
run 32 "$TMP/or.txt" "$TMP/or.log" --regs-per-thread "$((used + 1))"
[ "$(stats warps_per_sm "$TMP/or.log")" = "64" ] ||
  fail "vecadd uses r0-r$used, so ${used}+1 registers should still hit the cap"
echo "PASS 4/4: the register file caps occupancy only past 128 registers per thread"

echo "OK: chapter 19 gate passed"
