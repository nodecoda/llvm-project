#!/usr/bin/env bash
#
# Chapter 12 gate: execute GM32 on the Python model machine.
#
# Usage: check-ch12.sh <llvm-bin-dir>
#
# What each group proves:
#   1. the ELF loader agrees with llvm-objdump word for word, and the mini
#      static linker patches R_GPUMIN_21_PCREL calls against .symtab;
#   2. vecadd returns 4*i for the 32-lane launch and for a single warp;
#   3. loop, recursion and the seven special registers produce the values
#      predicted by the chapter 11 model;
#   4. the simulator fails loudly on a bad opcode, an undefined call target
#      and a non-uniform branch (divergence is chapter 14).
#
set -euo pipefail

BIN=${1:?usage: check-ch12.sh <llvm-bin-dir>}
HERE=$(cd "$(dirname "$0")" && pwd)
SIM="$HERE/../tools/gpumin_sim.py"
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

# --- 1. ELF loading, decoding and .text relocations -----------------------
llc -filetype=obj "$HERE/e2e/vecadd.ll" -o "$TMP/vecadd.o"
sim "$TMP/vecadd.o" --dump-text > "$TMP/vecadd.text"
"$BIN/llvm-objdump" -d "$TMP/vecadd.o" > "$TMP/vecadd.dump"
python3 - "$TMP/vecadd.dump" "$TMP/vecadd.text" <<'PY' || fail "decoder disagrees with llvm-objdump"
import re, sys
words = []
for line in open(sys.argv[1]):
    m = re.match(r'\s*([0-9a-f]+):\s+((?:[0-9a-f]{2} ){4})', line)
    if m:
        raw = bytes(int(b, 16) for b in m.group(2).split())
        words.append(int.from_bytes(raw, "little"))
got = [int(line, 16) for line in open(sys.argv[2])]
if not words:
    print("no words parsed from llvm-objdump", file=sys.stderr)
    sys.exit(1)
if words != got:
    for i, (a, b) in enumerate(zip(words, got)):
        if a != b:
            print("word %d: objdump %08x, loader %08x" % (i, a, b),
                  file=sys.stderr)
            break
    print("word count: objdump %d, loader %d" % (len(words), len(got)),
          file=sys.stderr)
    sys.exit(1)
PY
llc -filetype=obj "$HERE/e2e/loop.ll" -o "$TMP/loop.o"
sim "$TMP/loop.o" --dump-text > "$TMP/loop.text"
reloc=$(("$BIN/llvm-readobj" -r "$TMP/loop.o" |
  awk '$2 == "R_GPUMIN_21_PCREL" && $3 == "sum_to" {print $1; exit}') 2>/dev/null || true)
[ -n "$reloc" ] || fail "loop.o lost its call relocation"
sym=$(("$BIN/llvm-readobj" -s "$TMP/loop.o" |
  awk '/Name: sum_to \(/ {f=1; next} f && /Value:/ {print $2; exit}') 2>/dev/null || true)
[ -n "$sym" ] || fail "loop.o lost the sum_to symbol"
if ! python3 - "$TMP/loop.text" "$reloc" "$sym" <<'PY'
import sys
words = [int(line, 16) for line in open(sys.argv[1])]
pc = int(sys.argv[2], 0) // 4
target = int(sys.argv[3], 0) // 4
off = words[pc] & 0x1FFFFF
if off & (1 << 20):
    off -= 1 << 21
if pc + off != target:
    print("call at word %d targets %d, expected %d" % (pc, pc + off, target),
          file=sys.stderr)
    sys.exit(1)
PY
then
  fail "the mini linker did not resolve the call"
fi
echo "PASS 1/4: ELF loading, decoding and .text relocations agree"

# --- 2. vecadd ------------------------------------------------------------
llc -filetype=obj "$HERE/e2e/vecadd.ll" -o "$TMP/vecadd.o"
got=$(sim "$TMP/vecadd.o" --kernel vecadd --threads 32 \
        --fill 0x0=0,1,32 --fill 0x2000=0,3,32 \
        --arg 0x0 --arg 0x2000 --arg 0x4000 --arg 32 --dump 0x4000=32)
want=$(python3 - <<'PY'
print(" ".join(str(4 * i) for i in range(32)))
PY
)
[ "$got" = "$want" ] || fail "vecadd 32-lane result: $got"
got=$(sim "$TMP/vecadd.o" --kernel vecadd --threads 8 \
        --fill 0x0=0,1,8 --fill 0x2000=0,3,8 \
        --arg 0x0 --arg 0x2000 --arg 0x4000 --arg 8 --dump 0x4000=8)
[ "$got" = "0 4 8 12 16 20 24 28" ] || fail "vecadd one-warp result: $got"
# Two blocks re-run the same tid-addressed kernel; the result must not change.
got=$(sim "$TMP/vecadd.o" --kernel vecadd --blocks 2 --threads 32 \
        --fill 0x0=0,1,32 --fill 0x2000=0,3,32 \
        --arg 0x0 --arg 0x2000 --arg 0x4000 --arg 32 --dump 0x4000=32)
[ "$got" = "$want" ] || fail "vecadd with two blocks: $got"
echo "PASS 2/4: vecadd runs for 1 warp, 4 warps and 2 blocks"

# --- 3. loop, recursion and special registers -----------------------------
llc -filetype=obj "$HERE/e2e/loop.ll" -o "$TMP/loop.o"
got=$(sim "$TMP/loop.o" --kernel sum_kernel --arg 0x4000 --dump 0x4000=32)
want=$(python3 - <<'PY'
out = []
for warp in range(4):
    n = 8 + warp
    out += [str(n * (n - 1) // 2)] * 8
print(" ".join(out))
PY
)
[ "$got" = "$want" ] || fail "sum_kernel: $got"

llc -filetype=obj "$HERE/e2e/fib.ll" -o "$TMP/fib.o"
got=$(sim "$TMP/fib.o" --kernel fib_kernel --arg 0x4000 --dump 0x4000=32)
want=$(python3 - <<'PY'
def fib(n):
    a, b = 0, 1
    for _ in range(n):
        a, b = b, a + b
    return a
out = []
for warp in range(4):
    out += [str(fib(6 + warp))] * 8
print(" ".join(out))
PY
)
[ "$got" = "$want" ] || fail "fib_kernel: $got"

llc -filetype=obj "$HERE/e2e/special.ll" -o "$TMP/special.o"
sim "$TMP/special.o" --kernel special_kernel --blocks 2 --arg 0x4000 \
  --dump 0x4000=512 > "$TMP/special.txt"
python3 - "$TMP/special.txt" <<'PY' || fail "special registers do not match the model"
import sys
words = [int(x) for x in open(sys.argv[1]).read().split()]
if len(words) != 512:
    print("expected 512 words, got %d" % len(words), file=sys.stderr)
    sys.exit(1)
for gid in range(64):
    rec = words[gid * 8:(gid + 1) * 8]
    want = [gid % 8, (gid // 8) % 4, gid // 32, gid % 32, 32, 4, 255, 0]
    if rec != want:
        print("gid %d: got %s, want %s" % (gid, rec, want), file=sys.stderr)
        sys.exit(1)
PY
echo "PASS 3/4: loops, recursion and all seven special registers are correct"

# --- 4. error paths -------------------------------------------------------
cat > "$TMP/bad.s" <<'ASM'
	.text
	.globl bad
bad:
	.long 0
ASM
"$BIN/llvm-mc" -triple=gpumin -filetype=obj "$TMP/bad.s" -o "$TMP/bad.o"
if sim "$TMP/bad.o" --kernel bad > "$TMP/bad.log" 2>&1; then
  fail "a zero word was accepted as an instruction"
fi
grep -q 'unknown opcode 0x0' "$TMP/bad.log" || fail "bad opcode message"

cat > "$TMP/undef.s" <<'ASM'
	.text
	.globl caller
caller:
	call missing
	exit
ASM
"$BIN/llvm-mc" -triple=gpumin -filetype=obj "$TMP/undef.s" -o "$TMP/undef.o"
if sim "$TMP/undef.o" --kernel caller > "$TMP/undef.log" 2>&1; then
  fail "an undefined call target was accepted"
fi
grep -q 'undefined symbol' "$TMP/undef.log" || fail "undefined symbol message"

cat > "$TMP/divergent.ll" <<'IR'
target triple = "gpumin-unknown-elf"
declare i32 @llvm.gpumin.tid()
define i32 @sum_to(i32 %n) {
entry:
  br label %head
head:
  %i = phi i32 [ 0, %entry ], [ %next, %body ]
  %sum = phi i32 [ 0, %entry ], [ %add, %body ]
  %c = icmp slt i32 %i, %n
  br i1 %c, label %body, label %done
body:
  %add = add i32 %sum, %i
  %next = add i32 %i, 1
  br label %head
done:
  ret i32 %sum
}
define void @divergent(ptr addrspace(1) %out) #0 {
entry:
  %t = call i32 @llvm.gpumin.tid()
  %v = call i32 @sum_to(i32 %t)
  %p = getelementptr i32, ptr addrspace(1) %out, i32 %t
  store i32 %v, ptr addrspace(1) %p
  ret void
}
attributes #0 = { "gpumin_kernel" }
IR
llc -filetype=obj "$TMP/divergent.ll" -o "$TMP/divergent.o"
if sim "$TMP/divergent.o" --kernel divergent --arg 0x4000 \
     > "$TMP/divergent.log" 2>&1; then
  fail "a divergent warp was executed without a divergence stack"
fi
grep -q 'non-uniform branch' "$TMP/divergent.log" ||
  fail "divergence message"
echo "PASS 4/4: bad opcodes, undefined calls and divergence fail loudly"

echo "OK: chapter 12 gate passed"
