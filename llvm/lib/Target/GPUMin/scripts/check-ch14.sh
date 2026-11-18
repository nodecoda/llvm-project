#!/usr/bin/env bash
#
# Chapter 14 gate: intra-warp divergence and dynamic reconvergence.
#
# Usage: check-ch14.sh <llvm-bin-dir>
#
# What each group proves:
#   1. the divergence policy is in the shared model, and activemask is effectful
#      enough that the compiler keeps one read per control region;
#   2. a half-warp if/else runs both arms and reconverges with the full mask;
#   3. lanes that leave a loop at different iterations still add up, including
#      a nested diamond inside the loop;
#   4. --strict-uniform restores the chapter 12 one-PC model and rejects it.
#
set -euo pipefail

BIN=${1:?usage: check-ch14.sh <llvm-bin-dir>}
HERE=$(cd "$(dirname "$0")" && pwd)
SIM="$HERE/../tools/gpumin_sim.py"
MODEL="$HERE/../tools/gpumin_model.py"
JSON="$HERE/simt-model.json"
E2E="$HERE/e2e/divergence.ll"
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

# --- 1. model policy and the effectful activemask read -------------------
python3 - "$JSON" <<'PY' || fail "the divergence section in the model is wrong"
import json, sys
d = json.load(open(sys.argv[1]))["divergence"]
assert d["policy"] == "min-pc-frame-merge", d
assert d["strict_flag"] == "--strict-uniform", d
PY
python3 "$MODEL" --selftest > /dev/null || fail "the model self-test failed"

llc "$E2E" -o "$TMP/divergence.s"
llc -filetype=obj "$E2E" -o "$TMP/divergence.o"

# Three reads (two arms, one join) must survive as three instructions.  A pure
# read would be CSE'd into one that observes the pre-divergence mask, which is
# exactly the bug this chapter has to keep out.
reads=$(body diverge "$TMP/divergence.s" | grep -c 'sr.activemask' || true)
[ "$reads" -eq 3 ] || fail "expected 3 sr.activemask reads, saw $reads"
count=$(body diverge "$TMP/divergence.s" | grep -c 'bra p' || true)
[ "$count" -ge 1 ] || fail "the if/else was folded into straight-line code"
echo "PASS 1/4: the model names the policy and activemask is read per region"

# --- 2. one warp, two arms, one join ------------------------------------
sim "$TMP/divergence.o" --kernel diverge --threads 8 --arg 0x4000 \
  --dump 0x4000=32 > "$TMP/diverge.txt"
python3 - "$TMP/diverge.txt" <<'PY' || fail "the divergent warp record is wrong"
import sys
w = [int(x) for x in open(sys.argv[1]).read().split()]
if len(w) != 32:
    print("expected 32 words, got %d" % len(w), file=sys.stderr)
    sys.exit(1)
for t in range(8):
    tid, mask, value, join = w[t * 4:(t + 1) * 4]
    want_mask = 0x0F if t < 4 else 0xF0
    want_value = t * 2 if t < 4 else t + 100
    if (tid, mask, value, join) != (t, want_mask, want_value, 0xFF):
        print("lane %d: got %s, want %s" %
              (t, (tid, mask, value, join), (t, want_mask, want_value, 0xFF)),
              file=sys.stderr)
        sys.exit(1)
PY
echo "PASS 2/4: half the warp takes each arm and the join restores 0xff"

# --- 3. per-lane loop trips and a nested diamond --------------------------
sim "$TMP/divergence.o" --kernel triangle --threads 8 --arg 0x4000 \
  --dump 0x4000=8 > "$TMP/triangle.txt"
python3 - "$TMP/triangle.txt" <<'PY' || fail "triangle is wrong"
import sys
got = [int(x) for x in open(sys.argv[1]).read().split()]
want = [t * (t - 1) // 2 for t in range(8)]
if got != want:
    print("got %s, want %s" % (got, want), file=sys.stderr)
    sys.exit(1)
PY

cat > "$TMP/nested.ll" <<'IR'
target triple = "gpumin-unknown-elf"
declare i32 @llvm.gpumin.tid()
define void @nested(ptr addrspace(1) %out) #0 {
entry:
  %t = call i32 @llvm.gpumin.tid()
  br label %head
head:
  %i = phi i32 [ 0, %entry ], [ %next, %latch ]
  %acc = phi i32 [ 0, %entry ], [ %acc2, %latch ]
  %c = icmp ult i32 %i, %t
  br i1 %c, label %body, label %done
body:
  %odd = and i32 %i, 1
  %iseven = icmp eq i32 %odd, 0
  br i1 %iseven, label %plus, label %minus
plus:
  %a = add i32 %acc, %i
  br label %latch
minus:
  %s = sub i32 %acc, %i
  br label %latch
latch:
  %acc2 = phi i32 [ %a, %plus ], [ %s, %minus ]
  %next = add i32 %i, 1
  br label %head
done:
  %p = getelementptr i32, ptr addrspace(1) %out, i32 %t
  store i32 %acc, ptr addrspace(1) %p
  ret void
}
attributes #0 = { "gpumin_kernel" }
IR
llc -filetype=obj "$TMP/nested.ll" -o "$TMP/nested.o"
sim "$TMP/nested.o" --kernel nested --threads 8 --arg 0x4000 \
  --dump 0x4000=8 > "$TMP/nested.txt"
python3 - "$TMP/nested.txt" <<'PY' || fail "the nested diamond is wrong"
import sys
got = [int(x) for x in open(sys.argv[1]).read().split()]
want = [sum(i if i % 2 == 0 else -i for i in range(t)) for t in range(8)]
if got != want:
    print("got %s, want %s" % (got, want), file=sys.stderr)
    sys.exit(1)
PY
echo "PASS 3/4: loop exits at different iterations and a nested diamond agree"

# --- 4. the strict model still rejects divergence ------------------------
if sim "$TMP/divergence.o" --kernel diverge --threads 8 --arg 0x4000 \
     --dump 0x4000=32 --strict-uniform > "$TMP/strict.log" 2>&1; then
  fail "a divergent warp ran under --strict-uniform"
fi
grep -q 'non-uniform branch' "$TMP/strict.log" || fail "strict message"

# Under the strict model a uniform launch is still fine: every lane takes the
# same arm when the condition does not depend on the lane.
cat > "$TMP/uniform.ll" <<'IR'
target triple = "gpumin-unknown-elf"
define void @uniform(ptr addrspace(1) %out) #0 {
entry:
  store i32 7, ptr addrspace(1) %out
  ret void
}
attributes #0 = { "gpumin_kernel" }
IR
llc -filetype=obj "$TMP/uniform.ll" -o "$TMP/uniform.o"
got=$(sim "$TMP/uniform.o" --kernel uniform --threads 8 --arg 0x4000 \
        --dump 0x4000=1 --strict-uniform)
[ "$got" = "7" ] || fail "a uniform kernel was rejected by --strict-uniform: $got"
echo "PASS 4/4: --strict-uniform rejects divergence and still runs uniform code"

echo "OK: chapter 14 gate passed"
