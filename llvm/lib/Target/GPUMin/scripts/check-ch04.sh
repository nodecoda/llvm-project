#!/usr/bin/env bash
#
# Chapter 04 gate: integer ALU, predicate compares and predicated select.
#
# Usage: check-ch04.sh <llvm-bin-dir>
#
# What each group proves:
#   1. the eight ALU patterns pick the instruction whose funct matches the
#      DAG node -- including the three shifts, which are three different
#      nodes (shl / srl / sra) and three different funct values;
#   2. `icmp` + `select` becomes SETP + SELP, with the compare written to a
#      predicate register, not to a general register holding 0/1;
#   3. the unsigned flag (funct[3]) is actually used: icmp ugt and icmp sgt
#      must produce different opcodes;
#   4. the independent encoder agrees byte for byte on the new opcodes.
#
set -euo pipefail

BIN=${1:?usage: check-ch04.sh <llvm-bin-dir>}
HERE=$(cd "$(dirname "$0")" && pwd)
ENC="$HERE/../tools/gpumin-encode.py"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

cat > "$TMP/alu.ll" <<'IR'
target triple = "gpumin-unknown-elf"
define i32 @a_add(i32 %a, i32 %b) { %c = add  i32 %a, %b ret i32 %c }
define i32 @a_sub(i32 %a, i32 %b) { %c = sub  i32 %a, %b ret i32 %c }
define i32 @a_and(i32 %a, i32 %b) { %c = and  i32 %a, %b ret i32 %c }
define i32 @a_or (i32 %a, i32 %b) { %c = or   i32 %a, %b ret i32 %c }
define i32 @a_xor(i32 %a, i32 %b) { %c = xor  i32 %a, %b ret i32 %c }
define i32 @a_shl(i32 %a, i32 %b) { %c = shl  i32 %a, %b ret i32 %c }
define i32 @a_shr(i32 %a, i32 %b) { %c = lshr i32 %a, %b ret i32 %c }
define i32 @a_sar(i32 %a, i32 %b) { %c = ashr i32 %a, %b ret i32 %c }
define i32 @c_slt(i32 %a, i32 %b) { %c = icmp slt i32 %a, %b %d = select i1 %c, i32 %a, i32 %b ret i32 %d }
define i32 @c_ugt(i32 %a, i32 %b) { %c = icmp ugt i32 %a, %b %d = select i1 %c, i32 %a, i32 %b ret i32 %d }
define i32 @c_eq (i32 %a, i32 %b) { %c = icmp eq  i32 %a, %b %d = select i1 %c, i32 1, i32 0 ret i32 %d }
IR

"$BIN/llc" -march=gpumin -verify-machineinstrs "$TMP/alu.ll" -o "$TMP/alu.s" \
  || fail "llc rejected the ALU module"

# --- 1. ALU patterns --------------------------------------------------------
for pair in "a_add:add" "a_sub:sub" "a_and:and" "a_or:or" "a_xor:xor" \
            "a_shl:shl" "a_shr:shr" "a_sar:sar"; do
  fn=${pair%%:*}
  op=${pair##*:}
  body=$(sed -n "/^$fn:/,/Lfunc_end/p" "$TMP/alu.s")
  echo "$body" | grep -q "	$op r4, r4, r5" \
    || fail "$fn did not lower to \`$op r4, r4, r5\`"
done
echo "PASS 1/4: eight ALU nodes select eight different funct values"

# --- 2. SETP + SELP ---------------------------------------------------------
body=$(sed -n '/^c_slt:/,/Lfunc_end/p' "$TMP/alu.s")
echo "$body" | grep -q '	setp.lt p0, r4, r5' || fail "icmp slt did not become setp.lt"
echo "$body" | grep -q '	selp r4, p0, r4, r5'   || fail "select did not become selp"
echo "$body" | grep -q '	movi'                 && fail "the compare was materialised as 0/1 in a GPR"
echo "PASS 2/4: icmp + select becomes SETP + SELP with no 0/1 materialisation"

# --- 3. unsigned flag -------------------------------------------------------
body=$(sed -n '/^c_ugt:/,/Lfunc_end/p' "$TMP/alu.s")
echo "$body" | grep -q '	setp.ugt p0, r4, r5' || fail "icmp ugt did not set funct[3]"
body=$(sed -n '/^c_slt:/,/Lfunc_end/p' "$TMP/alu.s")
echo "$body" | grep -q '	setp.ugt' && fail "icmp slt produced an unsigned compare"
echo "PASS 3/4: the unsigned bit selects a different SETP encoding"

# --- 4. independent encoder -------------------------------------------------
cat > "$TMP/enc.s" <<'ASM'
add r4, r4, r5
sub r4, r4, r5
and r4, r4, r5
or r4, r4, r5
xor r4, r4, r5
shl r4, r4, r5
shr r4, r4, r5
sar r4, r4, r5
setp.eq p0, r4, r5
setp.ne p0, r4, r5
setp.lt p0, r4, r5
setp.le p0, r4, r5
setp.gt p0, r4, r5
setp.ge p0, r4, r5
setp.ueq p0, r4, r5
setp.une p0, r4, r5
setp.ult p0, r4, r5
setp.ule p0, r4, r5
setp.ugt p0, r4, r5
setp.uge p0, r4, r5
selp r4, p0, r4, r5
selp r31, p7, r1, r30
ASM

python3 "$ENC" --selftest > /dev/null || fail "the Python encoder self-test failed"
: > "$TMP/mc.txt"
while IFS= read -r line; do
  enc=$(printf '%s\n' "$line" | "$BIN/llvm-mc" -triple=gpumin -show-encoding 2>/dev/null \
        | grep -o '\[[^]]*\]' | tr -d '[]')
  [ -n "$enc" ] || fail "llvm-mc rejected: $line"
  echo "$enc" >> "$TMP/mc.txt"
done < "$TMP/enc.s"

python3 "$ENC" "$TMP/enc.s" > "$TMP/py.txt"
diff -u "$TMP/py.txt" "$TMP/mc.txt" > "$TMP/d.txt" || {
  cat "$TMP/d.txt" >&2
  fail "the independent encoder disagrees on the ALU/SETP/SELP encodings"
}
echo "PASS 4/4: $(wc -l < "$TMP/mc.txt") encodings match the independent encoder"

echo "OK: chapter 04 gate passed"
