#!/usr/bin/env bash
#
# Chapter 17 gate: the block barrier and what it fixes.
#
# Usage: check-ch17.sh <llvm-bin-dir>
#
# What each group proves:
#   1. `bar.sync` is one instruction described in one place, and two of them
#      are not merged;
#   2. a barrier sits between the store and the load it is supposed to order;
#   3. a cross-warp rotate is correct under both block schedules once the
#      barrier is present;
#   4. without the barrier the answer depends on the schedule -- so the
#      barrier is load-bearing, not decoration.
#
set -euo pipefail

BIN=${1:?usage: check-ch17.sh <llvm-bin-dir>}
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

fnbody() { # fnbody <name> <asm>: the lines of one function, label to .Lfunc_end
  awk -v f="$1:" '$0 ~ f {p = 1} p && /^\.Lfunc_end/ {exit} p' "$2"
}

run_rotate() { # run_rotate <object> <schedule> <outfile> <logfile>
  python3 "$SIM" "$1" --kernel rotate --blocks 1 --threads 32 \
    --arg 0x0 --arg 0x2000 --fill 0x0=100,1,32 --dump 0x2000=32 \
    --schedule "$2" --stats > "$3" 2> "$4"
}

# --- 1. one `bar.sync`, and two calls stay two instructions --------------
want=$'0x0b,0x00,0x00,0x48'
got=$(python3 "$TOOLS/gpumin_model.py" --encoding barrier)
[ "$got" = "$want" ] ||
  fail "model encodes bar.sync as [$got], want [$want]"

got=$(printf 'bar.sync\n' |
      "$BIN/llvm-mc" -triple=gpumin -show-encoding |
      grep -o '0x[0-9a-f][0-9a-f],[^]]*')
[ "$got" = "$want" ] ||
  fail "assembler encodes bar.sync as [$got], want [$want]"

cat > "$TMP/two.ll" <<'IR'
target triple = "gpumin-unknown-elf"
declare void @llvm.gpumin.barrier()
define void @k() #0 {
  call void @llvm.gpumin.barrier()
  call void @llvm.gpumin.barrier()
  ret void
}
attributes #0 = { "gpumin_kernel" }
IR
llc "$TMP/two.ll" -o "$TMP/two.s"
[ "$(grep -c '^[[:space:]]*bar\.sync' "$TMP/two.s")" = "2" ] ||
  fail "two barriers were merged or dropped"
echo "PASS 1/4: bar.sync is funct 11, 0x4800000b on the wire, and stays duplicated"

# --- 2. the barrier sits between the accesses it orders ------------------
cat > "$TMP/order.ll" <<'IR'
target triple = "gpumin-unknown-elf"
declare void @llvm.gpumin.barrier()
define i32 @between(ptr addrspace(2) %p, i32 %v) {
  store i32 %v, ptr addrspace(2) %p
  call void @llvm.gpumin.barrier()
  %x = load i32, ptr addrspace(2) %p
  ret i32 %x
}
IR
llc "$TMP/order.ll" -o "$TMP/order.s"
awk '/^between:/ {p = 1} p && /^\.Lfunc_end/ {exit} p' "$TMP/order.s" \
  > "$TMP/body.s"
st=$(grep -n 'st\.shared' "$TMP/body.s" | cut -d: -f1)
bar=$(grep -n 'bar\.sync' "$TMP/body.s" | cut -d: -f1)
ld=$(grep -n 'ld\.shared' "$TMP/body.s" | cut -d: -f1)
[ -n "$st" ] && [ -n "$bar" ] && [ -n "$ld" ] ||
  fail "between: missing st.shared / bar.sync / ld.shared"
[ "$st" -lt "$bar" ] && [ "$bar" -lt "$ld" ] ||
  fail "between: order is st=$st bar=$bar ld=$ld"

llc -filetype=obj "$E2E/barrier.ll" -o "$TMP/barrier.o"
llc "$E2E/barrier.ll" -o "$TMP/rotate.s"
[ "$(grep -c '^[[:space:]]*bar\.sync' "$TMP/rotate.s")" = "1" ] ||
  fail "rotate should contain exactly one barrier"
echo "PASS 2/4: the barrier stays between the shared store and the shared load"

# --- 3. with the barrier both schedules agree ---------------------------
want_rotate=$(python3 -c "print(' '.join(str(100 + (t + 8) % 32) for t in range(32)))")
for sched in interleaved greedy; do
  run_rotate "$TMP/barrier.o" "$sched" "$TMP/$sched.txt" "$TMP/$sched.log"
  got=$(cat "$TMP/$sched.txt")
  [ "$got" = "$want_rotate" ] ||
    fail "rotate/$sched: got [$got]"
  grep -q 'barriers=1' "$TMP/$sched.log" ||
    fail "rotate/$sched: $(grep -o 'barriers=[0-9]*' "$TMP/$sched.log")"
done
echo "PASS 3/4: a cross-warp rotate is correct under both block schedules"

# --- 4. without the barrier the answer depends on the schedule ----------
grep -v 'call void @llvm.gpumin.barrier()' "$E2E/barrier.ll" > "$TMP/racy.ll"
llc -filetype=obj "$TMP/racy.ll" -o "$TMP/racy.o"

run_rotate "$TMP/racy.o" interleaved "$TMP/racy-i.txt" "$TMP/racy-i.log"
got=$(cat "$TMP/racy-i.txt")
[ "$got" = "$want_rotate" ] ||
  fail "racy/interleaved changed; the interleaving model moved"
grep -q 'barriers=0' "$TMP/racy-i.log" ||
  fail "the racy build should report no barriers"

run_rotate "$TMP/racy.o" greedy "$TMP/racy-g.txt" "$TMP/racy-g.log"
got=$(cat "$TMP/racy-g.txt")
[ "$got" != "$want_rotate" ] ||
  fail "racy/greedy is still correct; the schedule no longer exposes the race"
first8=$(echo "$got" | cut -d' ' -f1-8)
[ "$first8" = "0 0 0 0 0 0 0 0" ] ||
  fail "racy/greedy: warp 0 should read slots nobody wrote, got [$first8]"
echo "PASS 4/4: without the barrier the result is a property of the schedule"

echo "OK: chapter 17 gate passed"
