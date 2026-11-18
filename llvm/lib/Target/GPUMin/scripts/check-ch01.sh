#!/usr/bin/env bash
#
# Chapter 01 gate: the GM32 registers and the R/I encoding formats.
#
# Usage: check-ch01.sh <llvm-bin-dir>
#
# What each group proves:
#   1. every architectural register name is bound to a register class, so the
#      AsmMatcher accepts it (this is the check that caught r1-r3 having no
#      allocation class at all);
#   2. the R format lays out opcode[31:26] | rd[25:21] | rs1[20:16] | rs2[15:11]
#      | funct[10:8] exactly as GPUMinInstrFormats.td says;
#   3. MOVI carries a sign-extended 16-bit immediate in [15:0];
#   4. LD/ST carry a 16-bit signed displacement in [15:0] (chapter 01 uses the
#      I format here; chapter 06 re-encodes them as the M format);
#   5. out-of-range immediates are rejected instead of silently truncated.
#
set -euo pipefail

BIN=${1:?usage: check-ch01.sh <llvm-bin-dir>}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

asm() {
  printf '%s\n' "$1" | "$BIN/llvm-mc" -triple=gpumin -show-encoding
}

# expect_enc <asm> <"0x..,0x..,0x..,0x..">
expect_enc() {
  local out
  out=$(asm "$1") || fail "assembler rejected: $1"
  echo "$out" | grep -qF "[$2]" || fail "$1 encoded as $(echo "$out" | grep -o '\[0x[0-9a-f,]*\]'), want [$2]"
}

# --- 1. register names ------------------------------------------------------
for i in $(seq 0 31); do
  asm "mov r$i, r5" > /dev/null || fail "r$i is not a valid operand"
done
for i in $(seq 0 7); do
  asm "setp.lt p$i, r1, r2" > /dev/null || fail "p$i is not a valid operand"
done
echo "PASS 1/5: r0-r31 and p0-p7 all parse"

# --- 2. R format ------------------------------------------------------------
expect_enc "mov r1, r2"       "0x00,0x00,0x22,0x04"
expect_enc "mov r5, r5"       "0x00,0x00,0xa5,0x04"
expect_enc "add r4, r5, r6"   "0x00,0x30,0x85,0x08"
expect_enc "sub r4, r5, r6"   "0x01,0x30,0x85,0x08"
expect_enc "and r4, r5, r6"   "0x02,0x30,0x85,0x08"
expect_enc "or  r4, r5, r6"   "0x03,0x30,0x85,0x08"
expect_enc "xor r4, r5, r6"   "0x04,0x30,0x85,0x08"
expect_enc "shl r4, r5, r6"   "0x05,0x30,0x85,0x08"
expect_enc "shr r4, r5, r6"   "0x06,0x30,0x85,0x08"
expect_enc "sar r4, r5, r6"   "0x07,0x30,0x85,0x08"
echo "PASS 2/5: R-format opcode/rd/rs1/rs2/funct fields encode in place"

# --- 3. MOVI ----------------------------------------------------------------
expect_enc "movi r3, 42"      "0x2a,0x00,0x60,0x14"
expect_enc "movi r1, -1"      "0xff,0xff,0x20,0x14"
echo "PASS 3/5: MOVI sign-extends its 16-bit immediate"

# --- 4. SETP / LD / ST ------------------------------------------------------
expect_enc "setp.lt p1, r2, r3" "0x02,0x18,0x22,0x0c"
expect_enc "setp.eq p1, r2, r3" "0x00,0x18,0x22,0x0c"
expect_enc "ld.global.b32 r7, 8(r8)"  "0x04,0x01,0xe8,0x20"
expect_enc "st.global.b32 r9, 4(r10)" "0x84,0x00,0x2a,0x25"
expect_enc "exit"               "0x00,0x00,0x00,0x48"
echo "PASS 4/5: SETP compares, LD/ST displacements and EXIT encode as specified"

# --- 5. range checks --------------------------------------------------------
printf 'movi r1, 70000\n' | "$BIN/llvm-mc" -triple=gpumin > /dev/null 2>&1 \
  && fail "movi accepted an immediate wider than 16 bits"
printf 'ld.global.b32 r1, 2000(r2)\n' | "$BIN/llvm-mc" -triple=gpumin > /dev/null 2>&1 \
  && fail "ld accepted a displacement wider than the 11-bit field"
echo "PASS 5/5: out-of-range immediates are rejected"

echo "OK: chapter 01 gate passed"
