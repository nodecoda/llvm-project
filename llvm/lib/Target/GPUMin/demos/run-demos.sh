#!/usr/bin/env bash
#
# GPUMin demo runner: one command, seven kernels, expected output checked.
#
# Usage: run-demos.sh <llvm-bin-dir> [demo-number ...]
#
# Each demo is compiled with llc (object + assembly), a few target
# instructions are printed, then the object runs in tools/gpumin_sim.py.
# The simulator writes dumped memory to stdout and statistics to stderr.
#
# The demo number is the file prefix, 01..07.  With no numbers every demo
# runs; `run-demos.sh <bin> 03 06` runs only those two.
#
set -uo pipefail

BIN=${1:-${LLVM_BIN:-}}
if [ -z "$BIN" ] || [ ! -x "$BIN/llc" ]; then
  echo "usage: run-demos.sh <llvm-bin-dir> [demo-number ...]" >&2
  echo "  e.g. run-demos.sh /home/ubuntu/llvm/builds/pr-ctpop/bin" >&2
  exit 2
fi
shift || true

HERE=$(cd "$(dirname "$0")" && pwd)
SIM="$HERE/../tools/gpumin_sim.py"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
export PYTHONDONTWRITEBYTECODE=1

FAILED=0
RAN=0

fail() {
  FAILED=$((FAILED + 1))
  echo "FAIL: $*" >&2
}

check() { # check <what> <want> <got>
  local what=$1 want=$2 got=$3
  [ "$want" = "$got" ] && return 0
  fail "$what: want [$want], got [$got]"
}

stat() { # stat <num> <key>: a scalar from the simulator's stderr line
  grep -o "$2=[0-9]*" "$TMP/$1.err" | head -1 | cut -d= -f2
}

run_demo() { # run_demo <num> <title> <kernel> <asm-grep> <want> [sim args...]
  local num=$1 title=$2 kernel=$3 asm=$4 want=$5
  shift 5
  local ll
  ll=$(echo "$HERE/$num"-*.ll)
  RAN=$((RAN + 1))
  echo
  echo "=== demo $num: $title ==="

  if ! "$BIN/llc" -march=gpumin -O1 -verify-machineinstrs -filetype=obj \
        "$ll" -o "$TMP/$num.o"; then
    fail "demo $num: llc could not compile $ll"
    return
  fi
  "$BIN/llc" -march=gpumin -O1 -verify-machineinstrs "$ll" -o "$TMP/$num.s"

  echo "--- target instructions matching /$asm/ ---"
  grep -E "$asm" "$TMP/$num.s" | sed 's/^[[:space:]]*/  /'

  if ! python3 "$SIM" "$TMP/$num.o" --kernel "$kernel" "$@" --stats \
        > "$TMP/$num.out" 2> "$TMP/$num.err"; then
    fail "demo $num: the simulator rejected the object"
    sed 's/^/  /' "$TMP/$num.err" >&2
    return
  fi

  echo "--- output (stdout) ---"
  echo "  $(tr '\n' ' ' < "$TMP/$num.out" | sed 's/ *$//')"
  echo "--- statistics (stderr) ---"
  sed 's/^/  /' "$TMP/$num.err"

  check "demo $num output" "$want" "$(cat "$TMP/$num.out")"
}

# --- 01: special registers ------------------------------------------------
# Every lane writes tid/laneid/warpid/blockid/ntid/nwarps/activemask/shared.
# Two blocks x 16 threads = 2 warps of 8 lanes = 32 records of 8 words.
want01=$(python3 - <<'PY'
recs = []
for b in range(2):
    for t in range(16):
        recs.append([t, t % 8, t // 8, b, 16, 2, 255, 0])
print(" ".join(str(w) for rec in recs for w in rec))
PY
)
run_demo 01 "special registers" ids 'sr\.' "$want01" \
  --blocks 2 --threads 16 --arg 0x0 --dump 0x0=256
check "demo 01 stores" 256 "$(stat 01 stores)"
check "demo 01 records" 256 "$(wc -w < "$TMP/01.out" | tr -d ' ')"

# --- 02: ALU and constants ------------------------------------------------
# 0x12345678 needs movi/movi/shl/or; 65535 needs the shl+shr pair because
# movi sign-extends; ashr becomes `sar`, not `shr`.
run_demo 02 "ALU and constants" alu 'movi|shl|shr|sar|and|xor|or ' \
  "-4211274 -339772576 -677642856 -1064911089 -1409296743 -1745355517 -2013945866 1852517302" \
  --blocks 1 --threads 8 --arg 0x0 --arg 0x200 \
  --fill 0x0=-1000000,123456789,8 --dump 0x200=8

# --- 03: uniform control flow --------------------------------------------
# Constant loop bounds and a condition that is the same in every lane:
# the warp never splits (splits=0, frames_max=1).
run_demo 03 "uniform control flow" control 'setp|bra' \
  "16 17 18 19 20 21 22 23" \
  --blocks 1 --threads 8 --arg 0x0 --arg 0x200 \
  --fill 0x0=1,1,8 --dump 0x200=8
check "demo 03 splits" 0 "$(stat 03 splits)"

# --- 04: memory widths ----------------------------------------------------
# u8/s8/u16/s16 come back as four arrays; the byte 0xff is 255 unsigned
# and -1 signed, the same bits read two ways.
run_demo 04 "memory widths" widths 'ld\.global|st\.global' \
  "255 95 191 31 127 223 63 159 -1 95 -65 31 127 -33 63 -97 65535 59999 54463 48927 43391 37855 32319 26783 -1 -5537 -11073 -16609 -22145 -27681 32319 26783" \
  --blocks 1 --threads 8 --arg 0x0 --arg 0x200 --arg 0x300 --arg 0x400 \
  --fill 0x0=-1,60000,8 --dump 0x400=32
check "demo 04 loads" 40 "$(stat 04 loads)"

# --- 05: device calls and the private stack -------------------------------
# The kernel calls @quad, which calls @twice twice: call/ret, lr in r31,
# and a 16-byte private frame that spills lr around the outer call.
run_demo 05 "device calls and stack" calls 'call|ret|st\.local|ld\.local|addi r30' \
  "1 13 25 37 49 61 73 85" \
  --blocks 1 --threads 8 --arg 0x0 --arg 0x200 \
  --fill 0x0=0,3,8 --dump 0x200=8
check "demo 05 local loads" 24 "$(stat 05 loc_ld)"

# --- 06: divergence -------------------------------------------------------
# The trip count is per-lane, so the warp really splits and the simulator
# keeps a frame stack; compare the splits/frames_max with demo 03.
run_demo 06 "divergence" divergent 'setp|bra' \
  "0 0 1 3 6 10 15 21" \
  --blocks 1 --threads 8 --arg 0x0 --arg 0x200 \
  --fill 0x0=0,1,8 --dump 0x200=8
[ "$(stat 06 splits)" -gt 0 ] ||
  fail "demo 06: expected the warp to split"
[ "$(stat 06 frames_max)" -gt 1 ] ||
  fail "demo 06: expected a frame stack deeper than one"

# --- 07: shared memory, barrier, block scope ------------------------------
# Two blocks of 32 lanes each sum 1..32 and 33..64; both sums are 528, so
# neither block can see the other's shared window.  The barrier sits between
# the shared stores and the shared loads.
run_demo 07 "shared memory and barrier" block_sum \
  'bar\.sync|ld\.shared|st\.shared|sr\.shared' "528 528" \
  --blocks 2 --threads 32 --arg 0x0 --arg 0x2000 \
  --fill 0x0=1,1,64 --dump 0x2000=2
check "demo 07 barriers" 2 "$(stat 07 barriers)"
check "demo 07 global stores" 2 "$(stat 07 glb_st)"
check "demo 07 shared stores" 64 "$(stat 07 shm_st)"

echo
if [ "$FAILED" -eq 0 ]; then
  echo "All $RAN demos passed."
else
  echo "$FAILED check(s) failed across $RAN demos." >&2
  exit 1
fi
