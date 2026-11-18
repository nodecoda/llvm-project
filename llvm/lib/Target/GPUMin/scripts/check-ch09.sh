#!/usr/bin/env bash
#
# Chapter 09 gate: special registers and target intrinsics.
#
# Usage: check-ch09.sh <llvm-bin-dir>
#
# What each group proves:
#   1. the seven special-register reads encode as SYS funct 2..8 and survive
#      an assemble/object/disassemble round trip; unknown selectors are
#      rejected instead of silently aliasing laneid;
#   2. each `llvm.gpumin.*` intrinsic selects exactly its `sr.<name>`
#      instruction, with no call left behind;
#   3. the reads are pure: two `laneid` calls in one function become one
#      instruction, and a kernel still ends in `exit`;
#   4. the whole module survives -verify-machineinstrs and the object file
#      equals the one assembled from the emitted .s.
#
set -euo pipefail

BIN=${1:?usage: check-ch09.sh <llvm-bin-dir>}
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

body() { # body <function> <asm-file>
  awk -v f="$1" '$0 ~ "^" f ":" {p=1} p{print} p && /^\.Lfunc_end/ {exit}' "$2"
}

# --- 1. encoding ----------------------------------------------------------
expect_enc "sr.laneid r1"     "0x02,0x00,0x20,0x48"
expect_enc "sr.warpid r1"     "0x03,0x00,0x20,0x48"
expect_enc "sr.blockid r1"    "0x04,0x00,0x20,0x48"
expect_enc "sr.tid r1"        "0x05,0x00,0x20,0x48"
expect_enc "sr.ntid r1"       "0x06,0x00,0x20,0x48"
expect_enc "sr.nwarps r1"     "0x07,0x00,0x20,0x48"
expect_enc "sr.activemask r1" "0x08,0x00,0x20,0x48"

# The destination is the ordinary 5-bit GPR field, not part of the funct.
expect_enc "sr.laneid r7"     "0x02,0x00,0xe0,0x48"
expect_enc "sr.activemask r31" "0x08,0x00,0xe0,0x4b"

# A selector is a fixed mnemonic: a typo must not quietly become laneid.
if printf 'sr.laneidfoo r1\n' \
     | "$BIN/llvm-mc" -triple=gpumin > /dev/null 2>&1; then
  fail "the assembler accepted an unknown special-register selector"
fi

{
  for i in laneid warpid blockid tid ntid nwarps activemask; do
    printf 'sr.%s r1\n' "$i"
  done
} > "$TMP/special.s"
"$BIN/llvm-mc" -triple=gpumin -filetype=obj "$TMP/special.s" -o "$TMP/special.o"
for i in laneid warpid blockid tid ntid nwarps activemask; do
  "$BIN/llvm-objdump" -d "$TMP/special.o" | grep -q "sr.$i r1" \
    || fail "sr.$i did not survive the object/disassemble round trip"
done
echo "PASS 1/4: seven SYS funct values encode, decode and reject typos"

# --- 2. intrinsic -> instruction -----------------------------------------
cat > "$TMP/intrinsics.ll" <<'IR'
target triple = "gpumin-unknown-elf"

declare i32 @llvm.gpumin.laneid()
declare i32 @llvm.gpumin.warpid()
declare i32 @llvm.gpumin.blockid()
declare i32 @llvm.gpumin.tid()
declare i32 @llvm.gpumin.ntid()
declare i32 @llvm.gpumin.nwarps()
declare i32 @llvm.gpumin.activemask()

define i32 @laneid() {
  %v = call i32 @llvm.gpumin.laneid()
  ret i32 %v
}
define i32 @warpid() {
  %v = call i32 @llvm.gpumin.warpid()
  ret i32 %v
}
define i32 @blockid() {
  %v = call i32 @llvm.gpumin.blockid()
  ret i32 %v
}
define i32 @tid() {
  %v = call i32 @llvm.gpumin.tid()
  ret i32 %v
}
define i32 @ntid() {
  %v = call i32 @llvm.gpumin.ntid()
  ret i32 %v
}
define i32 @nwarps() {
  %v = call i32 @llvm.gpumin.nwarps()
  ret i32 %v
}
define i32 @activemask() {
  %v = call i32 @llvm.gpumin.activemask()
  ret i32 %v
}
IR

"$BIN/llc" -march=gpumin -verify-machineinstrs "$TMP/intrinsics.ll" \
  -o "$TMP/intrinsics.s"

for name in laneid warpid blockid tid ntid nwarps activemask; do
  body "$name" "$TMP/intrinsics.s" | grep -q "sr.$name" \
    || fail "$name did not select sr.$name"
  if body "$name" "$TMP/intrinsics.s" | grep -q $'\tcall '; then
    fail "$name was lowered as a call instead of sr.$name"
  fi
done
echo "PASS 2/4: each intrinsic selects its own SYS instruction"

# --- 3. purity and kernel return -----------------------------------------
cat > "$TMP/pure.ll" <<'IR'
target triple = "gpumin-unknown-elf"

declare i32 @llvm.gpumin.laneid()
declare i32 @llvm.gpumin.activemask()

define i32 @twice() {
  %a = call i32 @llvm.gpumin.laneid()
  %b = call i32 @llvm.gpumin.laneid()
  %r = add i32 %a, %b
  ret i32 %r
}

define void @kernel_mask() #0 {
  %m = call i32 @llvm.gpumin.activemask()
  store i32 %m, ptr addrspace(1) null
  ret void
}

attributes #0 = { "gpumin_kernel" }
IR

"$BIN/llc" -march=gpumin -O2 -verify-machineinstrs "$TMP/pure.ll" \
  -o "$TMP/pure.s"

count=$(body twice "$TMP/pure.s" | grep -c 'sr.laneid' || true)
[ "$count" = 1 ] || fail "two pure laneid reads did not CSE (saw $count sr.laneid)"
body twice "$TMP/pure.s" | grep -q 'add r4, r1, r1' \
  || fail "the CSE'd laneid was not reused as both add operands"
body kernel_mask "$TMP/pure.s" | grep -q 'sr.activemask' \
  || fail "the kernel did not read activemask"
body kernel_mask "$TMP/pure.s" | grep -q $'\texit' \
  || fail "the kernel did not end in exit"
echo "PASS 3/4: special reads are pure, CSE to one instruction, and kernels exit"

# --- 4. object round trip -------------------------------------------------
"$BIN/llc" -march=gpumin -filetype=obj "$TMP/intrinsics.ll" -o "$TMP/a.o"
"$BIN/llvm-mc" -triple=gpumin -filetype=obj "$TMP/intrinsics.s" -o "$TMP/b.o"
"$BIN/llvm-objdump" -d -r "$TMP/a.o" | grep -v 'file format' > "$TMP/a.txt"
"$BIN/llvm-objdump" -d -r "$TMP/b.o" | grep -v 'file format' > "$TMP/b.txt"
diff -u "$TMP/a.txt" "$TMP/b.txt" > "$TMP/obj.diff" || {
  cat "$TMP/obj.diff" >&2
  fail "the codegen object and the reassembled object disagree"
}
echo "PASS 4/4: codegen object and reassembled .s are identical"

echo "OK: chapter 09 gate passed"
