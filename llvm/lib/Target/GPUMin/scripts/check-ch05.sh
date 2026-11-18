#!/usr/bin/env bash
#
# Chapter 05 gate: branches, labels and the PC-relative fixup.
#
# Usage: check-ch05.sh <llvm-bin-dir>
#
# What each group proves:
#   1. a label resolves through the fixup machinery, forward and backward,
#      and the displacement is counted in instruction words;
#   2. bit 25 separates `bra target` from `bra pN, target` -- p0 is not the
#      same encoding as "always";
#   3. basic blocks from IR become predicated and unconditional branches,
#      and the .o survives an assemble/disassemble/reassemble round trip;
#   4. an out-of-range displacement is reported, not truncated.
#
set -euo pipefail

BIN=${1:?usage: check-ch05.sh <llvm-bin-dir>}
HERE=$(cd "$(dirname "$0")" && pwd)
ENC="$HERE/../tools/gpumin-encode.py"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

# --- 1. labels and fixups ---------------------------------------------------
cat > "$TMP/fwd.s" <<'ASM'
bra .L1
exit
.L1:
bra p1, .L1
ASM
"$BIN/llvm-mc" -triple=gpumin -filetype=obj "$TMP/fwd.s" -o "$TMP/fwd.o" \
  || fail "the assembler rejected forward/backward branch labels"
"$BIN/llvm-objdump" -d "$TMP/fwd.o" > "$TMP/fwd.txt"
grep -q 'bra 2$' "$TMP/fwd.txt"      || fail "the forward branch is not a +2 word displacement"
grep -q 'bra p1, 0$' "$TMP/fwd.txt"  || fail "the backward branch is not a 0 word displacement"
echo "PASS 1/4: forward and backward labels resolve to word displacements"

# --- 2. the predicate bit ---------------------------------------------------
"$BIN/llvm-mc" -triple=gpumin -show-encoding "$TMP/fwd.s" > "$TMP/enc.txt"
grep -q '\[A,A,0x00,0x40\]' "$TMP/enc.txt" \
  || fail "bra with no predicate did not encode bit 25 as 0"
grep -q '\[A,A,0x40,0x42\]' "$TMP/enc.txt" \
  || fail "bra p1 did not set bit 25 and the predicate field"
echo "PASS 2/4: unconditional and predicated branches encode differently"

# --- 3. codegen -------------------------------------------------------------
cat > "$TMP/br.ll" <<'IR'
target triple = "gpumin-unknown-elf"
define i32 @loop(i32 %n) {
entry:
  %c = icmp sgt i32 %n, 0
  br i1 %c, label %body, label %done
body:
  %x = sub i32 %n, 1
  ret i32 %x
done:
  ret i32 0
}
IR

"$BIN/llc" -march=gpumin -verify-machineinstrs "$TMP/br.ll" -o "$TMP/br.s" \
  || fail "llc rejected the control-flow module"
grep -q '	bra p0, ' "$TMP/br.s" || fail "the conditional branch is not predicated"
# With analyzeBranch() reporting TBB/FBB correctly, block placement can make
# one successor the layout fallthrough, so an unconditional BRA is not
# required.  What must hold: the conditional target has a label, and both
# successors still return.
tgt=$(grep -o 'bra p0, [.]LBB[0-9_]*' "$TMP/br.s" | head -1 | awk '{print $3}')
[ -n "$tgt" ] || fail "the conditional branch has no local label target"
grep -q "^$tgt:" "$TMP/br.s" || fail "the conditional branch target has no label"
[ "$(grep -c $'^	ret$' "$TMP/br.s")" -eq 2 ] || fail "both successors must return"

"$BIN/llc" -march=gpumin -filetype=obj "$TMP/br.ll" -o "$TMP/br.o"
"$BIN/llvm-mc" -triple=gpumin -filetype=obj "$TMP/br.s" -o "$TMP/br_mc.o"
"$BIN/llvm-objdump" -d "$TMP/br.o"    | grep -v 'file format' > "$TMP/a.txt"
"$BIN/llvm-objdump" -d "$TMP/br_mc.o" | grep -v 'file format' > "$TMP/b.txt"
diff -u "$TMP/a.txt" "$TMP/b.txt" > "$TMP/d.txt" || {
  cat "$TMP/d.txt" >&2
  fail "the codegen object and the reassembled object disagree"
}
echo "PASS 3/4: basic blocks lower to branches and survive the round trip"

# --- 4. range check and independent encoding -------------------------------
printf 'bra 2\n' | "$BIN/llvm-mc" -triple=gpumin -filetype=obj -o /dev/null - \
  > /dev/null 2>&1 \
  && fail "a numeric branch target was accepted; only labels can be resolved"

printf 'bra .L1\n.space 160000\n.L1:\nexit\n' > "$TMP/far.s"
"$BIN/llvm-mc" -triple=gpumin -filetype=obj "$TMP/far.s" -o /dev/null - \
  > /dev/null 2>&1 \
  && fail "a branch displacement beyond +-32Ki words was accepted"

# The label object from step 1 must be byte-identical to what the independent
# encoder produces for the resolved displacements (2, 0).
python3 "$ENC" --selftest > /dev/null || fail "the Python encoder self-test failed"
printf 'bra 2\nexit\nbra p1, 0\n' > "$TMP/resolved.s"
EXPECT=$(python3 "$ENC" "$TMP/resolved.s" | sed 's/0x//g; s/,//g' | tr -d '\n')
GOT=$("$BIN/llvm-readobj" --hex-dump=.text "$TMP/fwd.o" \
      | grep -A1 'Hex dump' | tail -1 \
      | awk '{ for (i = 2; i <= NF && $i ~ /^[0-9a-fA-F]+$/; ++i) printf "%s", $i }')
[ "$EXPECT" = "$GOT" ] \
  || fail "resolved branch bytes are $GOT, the independent encoder says $EXPECT"
echo "PASS 4/4: out-of-range and non-label targets are rejected; bytes agree"

echo "OK: chapter 05 gate passed"
