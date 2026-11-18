#!/usr/bin/env bash
#
# Chapter 08 gate: type legalization, narrow memory and i64.
#
# Usage: check-ch08.sh <llvm-bin-dir>
#
# What each group proves:
#   1. i8/i16 loads select the narrow, extending M-format opcodes; a widened
#      value is never read as a full word;
#   2. i8/i16 stores keep their width, and i1 in memory is one byte holding 0
#      or 1 (a predicate has to be materialised first);
#   3. i64 is two i32 halves: little-endian load/store order, carry/borrow
#      arithmetic, and comparisons expanded from SETP/SELP;
#   4. the whole module survives -verify-machineinstrs and the object file
#      equals the one assembled from the emitted .s.
#
set -euo pipefail

BIN=${1:?usage: check-ch08.sh <llvm-bin-dir>}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

body() { # body <function> <asm-file>
  awk -v f="$1" '$0 ~ "^" f ":" {p=1} p{print} p && /^\.Lfunc_end/ {exit}' "$2"
}

cat > "$TMP/types.ll" <<'IR'
target triple = "gpumin-unknown-elf"

define i32 @ld8_z(ptr addrspace(1) %p) {
  %v = load i8, ptr addrspace(1) %p
  %r = zext i8 %v to i32
  ret i32 %r
}

define i32 @ld8_s(ptr addrspace(1) %p) {
  %v = load i8, ptr addrspace(1) %p
  %r = sext i8 %v to i32
  ret i32 %r
}

define i32 @ld16_z(ptr addrspace(1) %p) {
  %v = load i16, ptr addrspace(1) %p
  %r = zext i16 %v to i32
  ret i32 %r
}

define i32 @ld16_s(ptr addrspace(1) %p) {
  %v = load i16, ptr addrspace(1) %p
  %r = sext i16 %v to i32
  ret i32 %r
}

define void @st8(ptr addrspace(1) %p, i32 %v) {
  %t = trunc i32 %v to i8
  store i8 %t, ptr addrspace(1) %p
  ret void
}

define void @st16(ptr addrspace(1) %p, i32 %v) {
  %t = trunc i32 %v to i16
  store i16 %t, ptr addrspace(1) %p
  ret void
}

define i32 @ld_i1(ptr addrspace(1) %p) {
  %v = load i1, ptr addrspace(1) %p
  %r = zext i1 %v to i32
  ret i32 %r
}

define void @st_i1(ptr addrspace(1) %p, i32 %a, i32 %b) {
  %c = icmp slt i32 %a, %b
  store i1 %c, ptr addrspace(1) %p
  ret void
}

define i64 @ld64(ptr addrspace(1) %p) {
  %v = load i64, ptr addrspace(1) %p
  ret i64 %v
}

define void @st64(ptr addrspace(1) %p, i64 %v) {
  store i64 %v, ptr addrspace(1) %p
  ret void
}

define i64 @add64(i64 %a, i64 %b) {
  %r = add i64 %a, %b
  ret i64 %r
}

define i64 @sub64(i64 %a, i64 %b) {
  %r = sub i64 %a, %b
  ret i64 %r
}

define i32 @cmp64(i64 %a, i64 %b) {
  %c = icmp ult i64 %a, %b
  %r = zext i1 %c to i32
  ret i32 %r
}

define i32 @sext_arg(i16 %v) {
  %s = sext i16 %v to i32
  ret i32 %s
}

define i32 @widen(ptr addrspace(1) %p) {
  %v = load i16, ptr addrspace(1) %p
  %s = sext i16 %v to i32
  %z = zext i16 %v to i32
  %r = add i32 %s, %z
  ret i32 %r
}
IR

"$BIN/llc" -march=gpumin -verify-machineinstrs "$TMP/types.ll" -o "$TMP/types.s" \
  || fail "llc rejected the type module"

# --- 1. narrow loads -------------------------------------------------------
for pair in "ld8_z ld.global.u8" "ld8_s ld.global.s8" \
            "ld16_z ld.global.u16" "ld16_s ld.global.s16"; do
  set -- $pair
  body "$1" "$TMP/types.s" | grep -q "$2" \
    || fail "$1 did not select $2"
done
body sext_arg "$TMP/types.s" | grep -q $'\tshl ' \
  || fail "the register i16 sign extension did not use the shift pair"
body sext_arg "$TMP/types.s" | grep -q $'\tsar ' \
  || fail "the register i16 sign extension did not use the arithmetic shift"
body widen "$TMP/types.s" | grep -q 'ld.global.s16' \
  || fail "sext(load i16) did not fold into the sign-extending load"
body widen "$TMP/types.s" | grep -q $'\tand ' \
  || fail "zext(load i16) did not mask the value"
echo "PASS 1/4: narrow loads select u8/s8/u16/s16"

# --- 2. narrow stores and i1 ---------------------------------------------
body st8  "$TMP/types.s" | grep -q 'st.global.b8'  || fail "st8 lost its width"
body st16 "$TMP/types.s" | grep -q 'st.global.b16' || fail "st16 lost its width"
body st_i1 "$TMP/types.s" | grep -q 'selp' \
  || fail "storing i1 did not materialise the predicate as 0/1"
body st_i1 "$TMP/types.s" | grep -q 'st.global.b8' \
  || fail "storing i1 did not use a byte store"
body ld_i1 "$TMP/types.s" | grep -q 'ld.global.u8' \
  || fail "loading i1 did not read a byte"
body ld_i1 "$TMP/types.s" | grep -q $'\tand ' \
  || fail "loading i1 did not mask bit 0"
echo "PASS 2/4: narrow stores keep width and i1 is a byte"

# --- 3. i64 = two i32 halves ---------------------------------------------
low=$(body ld64 "$TMP/types.s" | grep -n 'ld.global.b32' | sed -n 1p)
high=$(body ld64 "$TMP/types.s" | grep -n 'ld.global.b32' | sed -n 2p)
[ -n "$low" ] && [ -n "$high" ] || fail "ld64 did not split into two loads"
body ld64 "$TMP/types.s" | sed -n 1p > /dev/null
printf '%s\n' "$low" | grep -q '0(r' || fail "ld64 low half is not at offset 0"
printf '%s\n' "$high" | grep -q '4(r' || fail "ld64 high half is not at offset 4"

body st64 "$TMP/types.s" | grep -q 'st.global.b32.*0(r' \
  || fail "st64 low half is not at offset 0"
body st64 "$TMP/types.s" | grep -q 'st.global.b32.*4(r' \
  || fail "st64 high half is not at offset 4"

body add64 "$TMP/types.s" | grep -q 'setp.ult' \
  || fail "i64 add did not compute the carry"
body sub64 "$TMP/types.s" | grep -q 'setp.ult' \
  || fail "i64 sub did not compute the borrow"
body cmp64 "$TMP/types.s" | grep -q 'setp.eq' \
  || fail "i64 comparison did not test the high words for equality"
body cmp64 "$TMP/types.s" | grep -q 'setp.ult' \
  || fail "i64 comparison did not compare unsigned"
echo "PASS 3/4: i64 is little-endian halves with carry and compare"

# --- 4. object round trip --------------------------------------------------
"$BIN/llc" -march=gpumin -filetype=obj "$TMP/types.ll" -o "$TMP/types.o"
"$BIN/llvm-mc" -triple=gpumin -filetype=obj "$TMP/types.s" -o "$TMP/types-mc.o"
"$BIN/llvm-objdump" -d -r "$TMP/types.o"    | grep -v 'file format' > "$TMP/a.txt"
"$BIN/llvm-objdump" -d -r "$TMP/types-mc.o" | grep -v 'file format' > "$TMP/b.txt"
diff -u "$TMP/a.txt" "$TMP/b.txt" > "$TMP/obj.diff" || {
  cat "$TMP/obj.diff" >&2
  fail "the codegen object and the reassembled object disagree"
}
echo "PASS 4/4: codegen object and reassembled .s are identical"

echo "OK: chapter 08 gate passed"
