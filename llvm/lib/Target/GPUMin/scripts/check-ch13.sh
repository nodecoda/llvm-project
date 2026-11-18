#!/usr/bin/env bash
#
# Chapter 13 gate: launch descriptors and the sr.args ABI.
#
# Usage: check-ch13.sh <llvm-bin-dir>
#
# What each group proves:
#   1. sr.args is SYS funct 9 at every layer: model, encoder, decoder, ISel;
#   2. a 2-block x 8-thread launch runs the argument-block vecadd correctly;
#   3. grid/block/args come from the descriptor, not from hidden constants,
#      and the chapter 12 register ABI still works;
#   4. malformed or missing launch state fails loudly.
#
set -euo pipefail

BIN=${1:?usage: check-ch13.sh <llvm-bin-dir>}
HERE=$(cd "$(dirname "$0")" && pwd)
SIM="$HERE/../tools/gpumin_sim.py"
MODEL="$HERE/../tools/gpumin_model.py"
JSON="$HERE/simt-model.json"
LAUNCH="$HERE/e2e/launch-vecadd.json"
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

body() { # body <function> <asm-file>
  awk -v f="$1" '$0 ~ "^" f ":" {p=1} p{print} p && /^\.Lfunc_end/ {exit}' "$2"
}

# --- 1. sr.args encoding, model and lowering ------------------------------
got=$(printf 'sr.args r1\n' | "$BIN/llvm-mc" -triple=gpumin -show-encoding |
      grep -o '\[[^]]*\]' | tr -d '[]')
[ "$got" = "0x09,0x00,0x20,0x48" ] || fail "sr.args encodes as [$got]"
want=$("$MODEL" --encoding args)
[ "$got" = "$want" ] || fail "model says sr.args is [$want], MC says [$got]"
python3 "$MODEL" --selftest > /dev/null || fail "the model self-test failed"
python3 - "$JSON" <<'PY' || fail "the launch section in the model is wrong"
import json, sys
launch = json.load(open(sys.argv[1]))["launch"]
assert launch["args_funct"] == 9, launch
assert launch["args_register"] == "ur0", launch
assert launch["grid_dims"] == 1, launch
PY

cat > "$TMP/args.ll" <<'IR'
target triple = "gpumin-unknown-elf"
declare i32 @llvm.gpumin.args()
define i32 @getargs() {
  %v = call i32 @llvm.gpumin.args()
  ret i32 %v
}
define void @k() #0 {
  %v = call i32 @llvm.gpumin.args()
  store i32 %v, ptr addrspace(1) null
  ret void
}
attributes #0 = { "gpumin_kernel" }
IR
llc "$TMP/args.ll" -o "$TMP/args.s"
body getargs "$TMP/args.s" | grep -q 'sr.args' ||
  fail "llvm.gpumin.args did not select sr.args"
if body getargs "$TMP/args.s" | grep -q $'\tcall '; then
  fail "llvm.gpumin.args was lowered as a call"
fi
body k "$TMP/args.s" | grep -q $'\texit' ||
  fail "a kernel reading sr.args did not end in exit"
echo "PASS 1/4: sr.args is funct 9 in the model, MC layer and ISel"

# --- 2. the 2 x 8 launch ------------------------------------------------
llc -filetype=obj "$HERE/e2e/launch.ll" -o "$TMP/launch.o"
llc "$HERE/e2e/launch.ll" -o "$TMP/launch.s"
got=$(sim "$TMP/launch.o" --kernel vecadd_launch --launch "$LAUNCH")
want=$(python3 - <<'PY'
print(" ".join(str(4 * i) for i in range(16)))
PY
)
[ "$got" = "$want" ] || fail "2x8 launch vecadd: $got"
body vecadd_launch "$TMP/launch.s" | grep -q 'sr.args' ||
  fail "the launch kernel does not read sr.args"
body vecadd_launch "$TMP/launch.s" | grep -q 'sr.blockid' ||
  fail "the launch kernel does not read blockid"
body vecadd_launch "$TMP/launch.s" | grep -q 'sr.tid' ||
  fail "the launch kernel does not read tid"
count=$(body vecadd_launch "$TMP/launch.s" | grep -c 'ld.global.b32' || true)
[ "$count" -ge 5 ] || fail "expected at least 5 global loads, saw $count"
body vecadd_launch "$TMP/launch.s" | grep -q 'st.global.b32' ||
  fail "the launch kernel does not store"
echo "PASS 2/4: 2 blocks x 8 threads run the argument-block vecadd"

# --- 3. descriptor-driven values and the legacy ABI ---------------------
cat > "$TMP/alt.json" <<'JSON'
{
  "grid": 2,
  "block": 8,
  "args": [0, 4096, 8192],
  "fill": [
    { "addr": 0, "start": 1, "step": 1, "count": 16 },
    { "addr": 4096, "start": 10, "step": 0, "count": 16 }
  ],
  "dump": [ { "addr": 8192, "count": 16 } ]
}
JSON
got=$(sim "$TMP/launch.o" --kernel vecadd_launch --launch "$TMP/alt.json")
want=$(python3 - <<'PY'
print(" ".join(str(11 + i) for i in range(16)))
PY
)
[ "$got" = "$want" ] || fail "alternate fills were not honoured: $got"

cat > "$TMP/one.json" <<'JSON'
{
  "grid": 1,
  "block": 8,
  "args": [0, 4096, 8192],
  "fill": [
    { "addr": 0, "start": 0, "step": 1, "count": 16 },
    { "addr": 4096, "start": 0, "step": 3, "count": 16 }
  ],
  "dump": [ { "addr": 8192, "count": 16 } ]
}
JSON
got=$(sim "$TMP/launch.o" --kernel vecadd_launch --launch "$TMP/one.json")
[ "$got" = "0 4 8 12 16 20 24 28 0 0 0 0 0 0 0 0" ] ||
  fail "grid=1 did not leave the second half untouched: $got"

llc -filetype=obj "$HERE/e2e/vecadd.ll" -o "$TMP/vecadd.o"
got=$(sim "$TMP/vecadd.o" --kernel vecadd --threads 8 \
        --fill 0x0=0,1,8 --fill 0x2000=0,3,8 \
        --arg 0x0 --arg 0x2000 --arg 0x4000 --arg 8 --dump 0x4000=8)
[ "$got" = "0 4 8 12 16 20 24 28" ] ||
  fail "the chapter 12 register ABI stopped working: $got"
echo "PASS 3/4: descriptor values are honoured and the legacy ABI still runs"

# --- 4. launch errors -----------------------------------------------------
if sim "$TMP/launch.o" --kernel vecadd_launch > "$TMP/nolaunch.log" 2>&1; then
  fail "sr.args was read without a launch descriptor"
fi
grep -q 'without a launch descriptor' "$TMP/nolaunch.log" ||
  fail "missing-launch message"

cat > "$TMP/badblock.json" <<'JSON'
{ "grid": 1, "block": 12, "args": [] }
JSON
if sim "$TMP/launch.o" --kernel vecadd_launch --launch "$TMP/badblock.json" \
     > "$TMP/badblock.log" 2>&1; then
  fail "a non-multiple-of-8 block was accepted"
fi
grep -q 'block must be a multiple of 8' "$TMP/badblock.log" ||
  fail "bad-block message"

cat > "$TMP/badgrid.json" <<'JSON'
{ "grid": 0, "block": 8, "args": [] }
JSON
if sim "$TMP/launch.o" --kernel vecadd_launch --launch "$TMP/badgrid.json" \
     > "$TMP/badgrid.log" 2>&1; then
  fail "grid=0 was accepted"
fi
grep -q 'grid must be a positive integer' "$TMP/badgrid.log" ||
  fail "bad-grid message"

cat > "$TMP/badargs.json" <<'JSON'
{ "grid": 1, "block": 8, "args_addr": 1048572, "args": [0, 0] }
JSON
if sim "$TMP/launch.o" --kernel vecadd_launch --launch "$TMP/badargs.json" \
     > "$TMP/badargs.log" 2>&1; then
  fail "an out-of-range argument block was accepted"
fi
grep -q 'do not fit in global memory' "$TMP/badargs.log" ||
  fail "bad-args message"
echo "PASS 4/4: missing and malformed launch state fail loudly"

echo "OK: chapter 13 gate passed"
