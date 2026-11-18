#!/usr/bin/env bash
#
# Chapter 16 gate: shared memory (address space 2).
#
# Usage: check-ch16.sh <llvm-bin-dir>
#
# What each group proves:
#   1. one `sr.shared` everywhere: the model, the assembler and instruction
#      selection agree on funct 10 and on the four encoded bytes;
#   2. the memory opcode follows the pointer's address space, and a shared
#      global never becomes an object-file symbol or relocation;
#   3. a real kernel round-trips through shared memory on the simulator;
#   4. static shared offsets are assigned in declaration order with alignment.
#
set -euo pipefail

BIN=${1:?usage: check-ch16.sh <llvm-bin-dir>}
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

# --- 1. `sr.shared` is one instruction, described in one place ------------
want=$'0x0a,0x00,0x20,0x48'
got=$(python3 "$TOOLS/gpumin_model.py" --encoding shared)
[ "$got" = "$want" ] ||
  fail "model encodes sr.shared as [$got], want [$want]"

got=$(printf 'sr.shared r1\n' |
      "$BIN/llvm-mc" -triple=gpumin -show-encoding |
      grep -o '0x[0-9a-f][0-9a-f],[^]]*')
[ "$got" = "$want" ] ||
  fail "assembler encodes sr.shared as [$got], want [$want]"

cat > "$TMP/sr.ll" <<'IR'
target triple = "gpumin-unknown-elf"
@w = internal addrspace(2) global [4 x i32] zeroinitializer, align 4
define i32 @read(i32 %i) {
  %p = getelementptr [4 x i32], ptr addrspace(2) @w, i32 0, i32 %i
  %v = load i32, ptr addrspace(2) %p
  ret i32 %v
}
IR
llc "$TMP/sr.ll" -o "$TMP/sr.s"
grep -q '\bsr\.shared\b' "$TMP/sr.s" ||
  fail "instruction selection did not use sr.shared for a shared global"
[ "$(grep -c '\bsr\.shared\b' "$TMP/sr.s")" = "1" ] ||
  fail "sr.shared was emitted more than once for one address"
echo "PASS 1/4: sr.shared is funct 10 in the model, 0x0a002048 on the wire, and what ISel emits"

# --- 2. the opcode follows the address space -----------------------------
cat > "$TMP/as.ll" <<'IR'
target triple = "gpumin-unknown-elf"
@tile = internal addrspace(2) global [8 x i32] zeroinitializer, align 4

; One access per address space per function: a store followed by a load of
; the same address would be forwarded at -O1 and prove nothing.
define void @copy_in(ptr addrspace(1) %g, ptr addrspace(2) %s) {
  %v = load i32, ptr addrspace(1) %g
  store i32 %v, ptr addrspace(2) %s
  ret void
}

define i32 @read_shared(ptr addrspace(2) %s) {
  %w = load i32, ptr addrspace(2) %s
  ret i32 %w
}

define void @write_global(ptr addrspace(1) %g, i32 %w) {
  store i32 %w, ptr addrspace(1) %g
  ret void
}

define void @touch_shared() {
  %p = getelementptr [8 x i32], ptr addrspace(2) @tile, i32 0, i32 1
  store i32 7, ptr addrspace(2) %p
  ret void
}
IR
llc "$TMP/as.ll" -o "$TMP/as.s"
[ "$(grep -c 'ld\.global\.b32' "$TMP/as.s")" = "1" ] ||
  fail "addrspace(1) should be exactly one ld.global.b32"
[ "$(grep -c 'st\.global\.b32' "$TMP/as.s")" = "1" ] ||
  fail "addrspace(1) should be exactly one st.global.b32"
[ "$(grep -c 'ld\.shared\.b32' "$TMP/as.s")" = "1" ] ||
  fail "addrspace(2) should be exactly one ld.shared.b32"
[ "$(grep -c 'st\.shared\.b32' "$TMP/as.s")" = "2" ] ||
  fail "addrspace(2) should be exactly two st.shared.b32"
grep -q 'ld\.local\|st\.local\|ld\.constant' "$TMP/as.s" &&
  fail "an unrelated address space leaked into the output"

llc "$TMP/as.ll" -filetype=obj -o "$TMP/as.o"
"$BIN/llvm-objdump" -t "$TMP/as.o" > "$TMP/syms.txt"
grep -q '\btile\b' "$TMP/syms.txt" &&
  fail "a shared global must not become an object-file symbol"
grep -q '\.comm[[:space:]]*tile' "$TMP/as.s" &&
  fail "a shared global must not reserve .bss storage"
"$BIN/llvm-objdump" -r "$TMP/as.o" > "$TMP/rels.txt"
grep -q '\btile\b' "$TMP/rels.txt" &&
  fail "nothing may relocate against a shared global"
echo "PASS 2/4: the space field follows the pointer, and shared globals are not in the image"

# --- 3. a real kernel round-trips through shared memory ------------------
llc -filetype=obj "$E2E/shared.ll" -o "$TMP/shared.o"
python3 "$SIM" "$TMP/shared.o" --kernel shared_copy --blocks 1 --threads 8 \
  --arg 0x0 --arg 0x2000 --fill 0x0=10,1,8 --dump 0x2000=8 --stats \
  > "$TMP/shared.txt" 2> "$TMP/shared.log"
python3 - "$TMP/shared.txt" <<'PY'
import sys
got = [int(x) for x in open(sys.argv[1]).read().split()]
want = list(range(17, 9, -1))
if got != want:
    print("got  %s" % got, file=sys.stderr)
    print("want %s" % want, file=sys.stderr)
    sys.exit(1)
PY
grep -q 'loads=16' "$TMP/shared.log" ||
  fail "shared_copy: $(grep -o 'loads=[0-9]*' "$TMP/shared.log") (want 8 global + 8 shared)"
grep -q 'stores=16' "$TMP/shared.log" ||
  fail "shared_copy: $(grep -o 'stores=[0-9]*' "$TMP/shared.log") (want 8 shared + 8 global)"
grep -q 'splits=0' "$TMP/shared.log" ||
  fail "shared_copy diverged; it is meant to be one lockstep warp"
echo "PASS 3/4: lanes exchange values through the shared window on the simulator"

# --- 4. static offsets are laid out in declaration order -----------------
cat > "$TMP/layout.ll" <<'IR'
target triple = "gpumin-unknown-elf"
@a = internal addrspace(2) global [8 x i32] zeroinitializer, align 4
@b = internal addrspace(2) global i8 zeroinitializer, align 1
@c = internal addrspace(2) global [4 x i32] zeroinitializer, align 16

define i32 @last_of_a() {
  %p = getelementptr [8 x i32], ptr addrspace(2) @a, i32 0, i32 7
  %v = load i32, ptr addrspace(2) %p
  ret i32 %v
}

define i32 @byte_b() {
  %v = load i8, ptr addrspace(2) @b
  %r = zext i8 %v to i32
  ret i32 %r
}

define i32 @first_of_c() {
  %p = getelementptr [4 x i32], ptr addrspace(2) @c, i32 0, i32 0
  %v = load i32, ptr addrspace(2) %p
  ret i32 %v
}
IR
llc "$TMP/layout.ll" -o "$TMP/layout.s"
fnbody last_of_a "$TMP/layout.s" | grep -q 'addi r[0-9]*, r[0-9]*, 28' ||
  fail "last_of_a: element 7 of a 32-byte array is at offset 28"
fnbody byte_b "$TMP/layout.s" | grep -q 'addi r[0-9]*, r[0-9]*, 32' ||
  fail "byte_b: @b follows @a, so it starts at 32"
fnbody first_of_c "$TMP/layout.s" | grep -q 'addi r[0-9]*, r[0-9]*, 48' ||
  fail "first_of_c: @c is align 16, so it starts at 48, not 33"
for f in last_of_a byte_b first_of_c; do
  fnbody "$f" "$TMP/layout.s" | grep -q 'ld\.shared\.' ||
    fail "$f: no shared load"
done
echo "PASS 4/4: shared offsets are aligned and follow declaration order"

echo "OK: chapter 16 gate passed"
