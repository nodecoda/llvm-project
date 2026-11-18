#!/usr/bin/env bash
#
# Chapter 07 gate: calls, the private stack and lr.
#
# Usage: check-ch07.sh <llvm-bin-dir>
#
# What each group proves:
#   1. `addi` has its own opcode, a signed 16-bit immediate, and is what
#      selection picks for `add x, <small constant>`;
#   2. `call` takes a symbol (never a number), stores a 21-bit displacement
#      counted in instruction words, resolves in place for local symbols,
#      emits R_GPUMIN_21_PCREL for linker-visible ones, and rejects targets
#      outside +-1Mi words;
#   3. the ABI split is real: device functions end in `ret`, kernels in
#      `exit`; a function that calls allocates a private frame, saves and
#      restores lr, and passes arguments in r4;
#   4. `llc`'s object and the object assembled from its `.s` are identical,
#      relocation records included.
#
set -euo pipefail

BIN=${1:?usage: check-ch07.sh <llvm-bin-dir>}
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

# --- 1. ADDI: I format, signed 16-bit, and the constant pattern ------------
expect_enc "addi r1, r2, -16"      "0xf0,0xff,0x22,0x18"
expect_enc "addi r31, r30, 1024"   "0x00,0x04,0xfe,0x1b"
expect_enc "addi r0, r0, 32767"    "0xff,0x7f,0x00,0x18"

for imm in 32767 -32768; do
  printf 'addi r1, r2, %s\n' "$imm" \
    | "$BIN/llvm-mc" -triple=gpumin > /dev/null 2>&1 \
    || fail "the assembler rejected an in-range addi immediate: $imm"
done
for imm in 32768 -32769; do
  printf 'addi r1, r2, %s\n' "$imm" \
    | "$BIN/llvm-mc" -triple=gpumin > /dev/null 2>&1 \
    && fail "the assembler accepted an out-of-range addi immediate: $imm"
done

cat > "$TMP/addi.ll" <<'IR'
target triple = "gpumin-unknown-elf"
define i32 @bump(i32 %x) {
  %r = add i32 %x, 100
  ret i32 %r
}
IR
"$BIN/llc" -march=gpumin -verify-machineinstrs "$TMP/addi.ll" \
  -o "$TMP/addi.s" || fail "llc rejected the add-by-constant module"
grep -q 'addi r4, r4, 100' "$TMP/addi.s" \
  || fail "add x, 100 did not select addi"
grep -q 'movi' "$TMP/addi.s" \
  && fail "add x, 100 materialised the constant instead of folding it"
echo "PASS 1/4: ADDI encodes, checks its range and folds small constants"

# --- 2. CALL: symbol operand, 21-bit word displacement, relocations -------
cat > "$TMP/call-local.s" <<'ASM'
	.text
	.globl entry
entry:
	call tgt
	exit
tgt:
	exit
ASM
"$BIN/llvm-mc" -triple=gpumin -filetype=obj "$TMP/call-local.s" \
  -o "$TMP/call-local.o" || fail "the assembler rejected a local call"
"$BIN/llvm-objdump" -d "$TMP/call-local.o" > "$TMP/local.dis"
grep -q $'\tcall 2$' "$TMP/local.dis" \
  || fail "a local call two words ahead did not encode as call 2"

printf 'call 3\n' | "$BIN/llvm-mc" -triple=gpumin -filetype=obj -o /dev/null \
  > /dev/null 2>&1 && fail "a numeric call target was accepted"

cat > "$TMP/far.s" <<'ASM'
	.text
	call far
	.space 8388608
far:
	exit
ASM
"$BIN/llvm-mc" -triple=gpumin -filetype=obj "$TMP/far.s" -o "$TMP/far.o" \
  > /dev/null 2>&1 && fail "an out-of-range call target was accepted"

cat > "$TMP/call-ext.ll" <<'IR'
target triple = "gpumin-unknown-elf"
define i32 @add1(i32 %x) {
  %r = add i32 %x, 1
  ret i32 %r
}
define i32 @caller(i32 %x) {
  %r = call i32 @add1(i32 %x)
  ret i32 %r
}
IR
"$BIN/llc" -march=gpumin -filetype=obj "$TMP/call-ext.ll" \
  -o "$TMP/call-ext.o" || fail "llc could not build a calling module"
"$BIN/llvm-readobj" -r "$TMP/call-ext.o" > "$TMP/relocs.txt"
grep -q 'R_GPUMIN_21_PCREL add1' "$TMP/relocs.txt" \
  || fail "a call to a global symbol did not emit R_GPUMIN_21_PCREL"
echo "PASS 2/4: CALL is symbolic, word-scaled and range-checked"

# --- 3. ret vs exit, frames, lr -------------------------------------------
cat > "$TMP/abi.ll" <<'IR'
target triple = "gpumin-unknown-elf"

define i32 @add1(i32 %x) {
  %r = add i32 %x, 1
  ret i32 %r
}

define i32 @fib(i32 %n) {
  %c = icmp slt i32 %n, 2
  br i1 %c, label %base, label %rec
base:
  ret i32 %n
rec:
  %n1 = sub i32 %n, 1
  %n2 = sub i32 %n, 2
  %a = call i32 @fib(i32 %n1)
  %b = call i32 @fib(i32 %n2)
  %r = add i32 %a, %b
  ret i32 %r
}

define i32 @kernel(ptr addrspace(1) %p) #0 {
  %x = load volatile i32, ptr addrspace(1) %p
  %y = call i32 @fib(i32 %x)
  ret i32 %y
}

attributes #0 = { "gpumin_kernel" }
IR
"$BIN/llc" -march=gpumin -verify-machineinstrs "$TMP/abi.ll" \
  -o "$TMP/abi.s" || fail "llc rejected the ABI module"

[ "$(grep -c '^	ret$' "$TMP/abi.s")" -eq 2 ] \
  || fail "add1 and fib must return with ret"
[ "$(grep -c '^	exit$' "$TMP/abi.s")" -eq 1 ] \
  || fail "the kernel must end with exit"

grep -qE 'addi r30, r30, -[0-9]+' "$TMP/abi.s" \
  || fail "a calling function did not allocate a private frame"
grep -q 'st.local.b32 r31' "$TMP/abi.s" \
  || fail "lr was not saved on entry"
grep -q 'ld.local.b32 r31' "$TMP/abi.s" \
  || fail "lr was not restored before ret"
[ "$(grep -c $'\tcall fib$' "$TMP/abi.s")" -eq 3 ] \
  || fail "fib's two recursive calls and the kernel's call were not emitted"
grep -qE 'addi r4, r2[0-9], -2' "$TMP/abi.s" \
  || fail "the second recursive argument did not travel in r4"
echo "PASS 3/4: ret/exit split, private frame and lr preservation"

# --- 4. codegen object == reassembled object ------------------------------
"$BIN/llc" -march=gpumin -filetype=obj "$TMP/abi.ll" -o "$TMP/abi.o"
"$BIN/llvm-mc" -triple=gpumin -filetype=obj "$TMP/abi.s" -o "$TMP/abi-mc.o"
"$BIN/llvm-objdump" -d -r "$TMP/abi.o"    | grep -v 'file format' > "$TMP/a.txt"
"$BIN/llvm-objdump" -d -r "$TMP/abi-mc.o" | grep -v 'file format' > "$TMP/b.txt"
diff -u "$TMP/a.txt" "$TMP/b.txt" > "$TMP/obj.diff" || {
  cat "$TMP/obj.diff" >&2
  fail "the codegen object and the reassembled object disagree"
}
echo "PASS 4/4: codegen object and reassembled .s are identical"

echo "OK: chapter 07 gate passed"
