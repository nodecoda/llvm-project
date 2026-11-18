#!/usr/bin/env bash
#
# Chapter 02 gate: the MC layer is bidirectional.
#
# Usage: check-ch02.sh <llvm-bin-dir>
#
# What each group proves:
#   1. an independent encoder (tools/gpumin-encode.py, written from the ISA
#      description, sharing no code with LLVM) produces the same bytes as
#      llvm-mc for the whole chapter-01 instruction set;
#   2. every word that llvm-mc emits disassembles back to the same mnemonic;
#   3. assembling that disassembly reproduces the original bytes (the round
#      trip is an identity, not just "some output");
#   4. a word the decoder does not know is reported as one invalid
#      instruction, not silently decoded or resynchronised.
#
set -euo pipefail

BIN=${1:?usage: check-ch02.sh <llvm-bin-dir>}
HERE=$(cd "$(dirname "$0")" && pwd)
ENC="$HERE/../tools/gpumin-encode.py"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

bytes_of() {
  "$BIN/llvm-mc" -triple=gpumin -show-encoding 2>/dev/null \
    | grep -o '\[[^]]*\]' | tr -d '[]'
}

cat > "$TMP/corpus.s" <<'ASM'
mov r0, r0
mov r1, r2
mov r31, r30
movi r0, 0
movi r3, 42
movi r31, -1
add r4, r5, r6
sub r7, r8, r9
and r10, r11, r12
or r13, r14, r15
xor r16, r17, r18
shl r19, r20, r21
shr r22, r23, r24
sar r25, r26, r27
setp.eq p0, r1, r2
setp.lt p7, r30, r31
ld r7, 8(r8)
ld r1, -4(r2)
ld r31, 32767(r30)
st r9, 4(r10)
st r0, 0(r1)
st r31, -32768(r30)
exit
ASM

# --- 1. independent encoder ------------------------------------------------
python3 "$ENC" --selftest || fail "the Python encoder failed its self-test"

: > "$TMP/mc.txt"
while IFS= read -r line; do
  enc=$(printf '%s\n' "$line" | bytes_of)
  [ -n "$enc" ] || fail "llvm-mc rejected: $line"
  echo "$enc" >> "$TMP/mc.txt"
done < "$TMP/corpus.s"

python3 "$ENC" "$TMP/corpus.s" > "$TMP/py.txt"
if ! diff -u "$TMP/py.txt" "$TMP/mc.txt" > "$TMP/diff.txt"; then
  cat "$TMP/diff.txt" >&2
  fail "the independent encoder disagrees with llvm-mc"
fi
echo "PASS 1/4: $(wc -l < "$TMP/mc.txt") encodings match the independent encoder"

# --- 2. decode -------------------------------------------------------------
: > "$TMP/dis.txt"
while IFS= read -r enc; do
  printf '%s\n' "$enc" | tr ',' ' ' >> "$TMP/dis.txt"
done < "$TMP/mc.txt"

"$BIN/llvm-mc" -triple=gpumin -disassemble < "$TMP/dis.txt" \
  > "$TMP/rt.s" 2> "$TMP/rt.err"
[ -s "$TMP/rt.s" ] || fail "the disassembler produced no output"
grep -q 'invalid instruction' "$TMP/rt.err" \
  && fail "the disassembler rejected a word that llvm-mc had just encoded"
[ "$(grep -c . "$TMP/rt.s")" -eq "$(wc -l < "$TMP/mc.txt")" ] \
  || fail "the disassembler did not return one line per word"
echo "PASS 2/4: every encoded word decodes to one instruction"

# --- 3. round trip ---------------------------------------------------------
: > "$TMP/rt.txt"
while IFS= read -r line; do
  [ -z "$line" ] && continue
  enc=$(printf '%s\n' "$line" | bytes_of)
  [ -n "$enc" ] || fail "re-assembling the disassembly failed on: $line"
  echo "$enc" >> "$TMP/rt.txt"
done < "$TMP/rt.s"

diff -u "$TMP/mc.txt" "$TMP/rt.txt" > "$TMP/rt.diff" || {
  cat "$TMP/rt.diff" >&2
  fail "asm -> bytes -> asm -> bytes is not an identity"
}
echo "PASS 3/4: asm -> bytes -> asm -> bytes is an identity"

# --- 4. unknown words ------------------------------------------------------
out=$(printf '0xff 0xff 0xff 0xff\n' | "$BIN/llvm-mc" -triple=gpumin -disassemble 2>&1)
echo "$out" | grep -q 'invalid instruction encoding' \
  || fail "an unknown word was not reported as invalid"
echo "$out" | grep -q '^0xff 0xff 0xff 0xff$' \
  || fail "an unknown word printed something other than the input bytes"
echo "PASS 4/4: unknown words are reported once, without resynchronising"

echo "OK: chapter 02 gate passed"
