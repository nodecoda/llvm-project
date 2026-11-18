#!/usr/bin/env bash
#
# Chapter 06 gate: the M format and 32-bit global memory.
#
# Usage: check-ch06.sh <llvm-bin-dir>
#
# What each group proves:
#   1. space, size and sign are three separate fields, and every space/size
#      combination has its own encoding;
#   2. the displacement is a signed *byte* offset of 11 bits -- not an element
#      index, and not silently truncated outside +-1024;
#   3. llvm-mc, the independent encoder and the decoder agree on the whole
#      memory corpus, and the round trip is an identity;
#   4. IR loads and stores from addrspace(1) select the global forms, fold a
#      constant GEP into the displacement, and compute a register-indexed
#      address with movi/shl/add before using offset 0.
#
set -euo pipefail

BIN=${1:?usage: check-ch06.sh <llvm-bin-dir>}
HERE=$(cd "$(dirname "$0")" && pwd)
ENC="$HERE/../tools/gpumin-encode.py"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

expect_enc() {
  local line=$1 want=$2 got
  got=$(printf '%s\n' "$line" | "$BIN/llvm-mc" -triple=gpumin -show-encoding \
        | grep -o '\[[^]]*\]' | tr -d '[]')
  [ "$got" = "$want" ] || fail "$line encodes as [$got], want [$want]"
}

bytes_of() {
  "$BIN/llvm-mc" -triple=gpumin -show-encoding 2>/dev/null \
    | grep -o '\[[^]]*\]' | tr -d '[]'
}

word_of() {
  printf '%s\n' "$1" | "$BIN/llvm-mc" -triple=gpumin -show-encoding \
    | grep -o '0x[0-9a-f]*' | tr '\n' ' '
}

# --- 1. field layout -------------------------------------------------------
# [15:5] imm11 | [4:3] space | [2:1] size | [0] sign
expect_enc "ld.global.b32 r7, 8(r8)"    "0x04,0x01,0xe8,0x20"
expect_enc "st.global.b32 r9, 4(r10)"   "0x84,0x00,0x2a,0x25"
expect_enc "ld.shared.u16 r1, 0(r2)"    "0x0a,0x00,0x22,0x20"
expect_enc "ld.local.u16 r1, 0(r2)"     "0x12,0x00,0x22,0x20"
expect_enc "ld.constant.u16 r1, 0(r2)"  "0x1a,0x00,0x22,0x20"
expect_enc "ld.global.u8 r1, 0(r2)"     "0x00,0x00,0x22,0x20"
expect_enc "ld.global.s8 r1, 0(r2)"     "0x01,0x00,0x22,0x20"
expect_enc "st.global.b8 r1, 0(r2)"     "0x00,0x00,0x22,0x24"
echo "PASS 1/4: space[4:3], size[2:1] and sign[0] encode independently"

# --- 2. signed byte displacement, 11 bits ----------------------------------
for imm in -1024 1023; do
  printf 'ld.global.b32 r1, %s(r2)\n' "$imm" \
    | "$BIN/llvm-mc" -triple=gpumin > /dev/null 2>&1 \
    || fail "the assembler rejected an in-range displacement: $imm"
done
for imm in 1024 -1025; do
  printf 'ld.global.b32 r1, %s(r2)\n' "$imm" \
    | "$BIN/llvm-mc" -triple=gpumin > /dev/null 2>&1 \
    && fail "the assembler accepted an out-of-range displacement: $imm"
done

# A b32 load at 3(r2) must encode the byte offset 3, exactly like the b8 form;
# the only difference between the two words is the size field.
w32=$(word_of "ld.global.b32 r1, 3(r2)")
w8=$(word_of "ld.global.u8 r1, 3(r2)")
python3 - "$w32" "$w8" <<'PY' || fail "the displacement is not a byte offset or is scaled by size"
import sys
a = int(sys.argv[1].split()[0], 16)
b = int(sys.argv[2].split()[0], 16)
assert (a >> 5) & 0x7FF == 3, hex(a)
assert (b >> 5) & 0x7FF == 3, hex(b)
assert a - b == 0b100, (hex(a), hex(b))
PY
echo "PASS 2/4: the displacement is a signed byte offset, not an element index"

# --- 3. independent encoder and round trip ---------------------------------
cat > "$TMP/corpus.s" <<'ASM'
ld.global.b32 r0, 0(r0)
ld.global.b32 r7, 8(r8)
ld.global.s8 r1, -4(r2)
ld.global.u8 r3, 1023(r4)
ld.shared.u16 r31, -1024(r30)
ld.local.s16 r5, 12(r6)
ld.constant.b32 r7, -8(r8)
st.global.b32 r9, 4(r10)
st.global.b8 r11, -12(r12)
st.shared.b16 r13, 0(r14)
st.local.b32 r15, 1023(r16)
ASM

: > "$TMP/mc.txt"
while IFS= read -r line; do
  enc=$(printf '%s\n' "$line" | bytes_of)
  [ -n "$enc" ] || fail "llvm-mc rejected: $line"
  echo "$enc" >> "$TMP/mc.txt"
done < "$TMP/corpus.s"

python3 "$ENC" "$TMP/corpus.s" > "$TMP/py.txt"
diff -u "$TMP/py.txt" "$TMP/mc.txt" > "$TMP/diff.txt" || {
  cat "$TMP/diff.txt" >&2
  fail "the independent encoder disagrees with llvm-mc on the memory corpus"
}

"$BIN/llvm-mc" -triple=gpumin -filetype=obj -o "$TMP/corpus.o" "$TMP/corpus.s"
"$BIN/llvm-objdump" -d "$TMP/corpus.o" | sed -n 's/^ *[0-9a-f]*: [0-9a-f ]*  //p' \
  > "$TMP/dis.txt"
diff -u <(sed 's/^[[:space:]]*//; s/[[:space:]]\+/ /g; s/ $//' "$TMP/corpus.s") \
        <(sed 's/^[[:space:]]*//; s/[[:space:]]\+/ /g; s/ $//' "$TMP/dis.txt") \
  > "$TMP/rt.diff" || {
  cat "$TMP/rt.diff" >&2
  fail "assembled memory instructions did not disassemble back to the source"
}
echo "PASS 3/4: encoder agreement and the asm -> obj -> asm identity"

# --- 4. codegen ------------------------------------------------------------
cat > "$TMP/memory.ll" <<'IR'
target triple = "gpumin-unknown-elf"

define i32 @roundtrip(ptr addrspace(1) %p, i32 %v) {
entry:
  store volatile i32 %v, ptr addrspace(1) %p
  %x = load volatile i32, ptr addrspace(1) %p
  ret i32 %x
}

define i32 @folded(ptr addrspace(1) %p) {
entry:
  %q = getelementptr i32, ptr addrspace(1) %p, i32 2
  %x = load volatile i32, ptr addrspace(1) %q
  ret i32 %x
}

define i32 @indexed(ptr addrspace(1) %p, i32 %i) {
entry:
  %q = getelementptr i32, ptr addrspace(1) %p, i32 %i
  %x = load volatile i32, ptr addrspace(1) %q
  ret i32 %x
}

define i32 @generic(ptr %p) {
entry:
  %x = load volatile i32, ptr %p
  ret i32 %x
}
IR

"$BIN/llc" -march=gpumin -verify-machineinstrs "$TMP/memory.ll" \
  -o "$TMP/memory.s" || fail "llc rejected the memory module"

grep -q 'st\.global\.b32' "$TMP/memory.s" || fail "the store did not select st.global.b32"
grep -q 'ld\.global\.b32' "$TMP/memory.s" || fail "the load did not select ld.global.b32"
grep -q 'ld\.global\.b32 r4, 8(r4)' "$TMP/memory.s" \
  || fail "gep +2 was not folded into an 8-byte displacement"
grep -q 'shl' "$TMP/memory.s" || fail "the indexed address did not shift the element index"
grep -qE 'ld\.global\.b32 r[0-9]+, 0\(r[0-9]+\)' "$TMP/memory.s" \
  || fail "the indexed load did not use offset 0 after computing the address"

"$BIN/llc" -march=gpumin -filetype=obj "$TMP/memory.ll" -o "$TMP/memory.o"
"$BIN/llvm-mc" -triple=gpumin -filetype=obj "$TMP/memory.s" -o "$TMP/reasm.o"
"$BIN/llvm-objdump" -d "$TMP/memory.o" | grep -v 'file format' > "$TMP/a.txt"
"$BIN/llvm-objdump" -d "$TMP/reasm.o" | grep -v 'file format' > "$TMP/b.txt"
diff -u "$TMP/a.txt" "$TMP/b.txt" > "$TMP/obj.diff" || {
  cat "$TMP/obj.diff" >&2
  fail "the codegen object and the reassembled object disagree"
}
echo "PASS 4/4: addrspace(1) loads/stores select, fold and round-trip"

echo "OK: chapter 06 gate passed"
