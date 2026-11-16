#!/usr/bin/env bash
#
# Chapter 2 gate: prove that the RVMin MC layer encodes RV32I exactly like the
# reference RISC-V assembler.
#
# Usage: check-rvmin-encodings.sh <llvm-bin-dir>
#
# Three independent checks:
#   1. llvm-mc -triple=rvmin and -triple=riscv32 produce the same encoding
#      bytes for every instruction in rvmin-encoding-corpus.s.
#   2. The .text section of the object produced by RVMin is byte-identical to
#      the reference object for the same source.
#   3. llvm-readelf recognizes the RVMin object as a 32-bit RISC-V ELF file.
#
set -euo pipefail

BIN=${1:?usage: check-rvmin-encodings.sh <llvm-bin-dir>}
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
CORPUS="$HERE/rvmin-encoding-corpus.s"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

# 1. Encoding-level diff.
"$BIN/llvm-mc" -triple=rvmin -show-encoding "$CORPUS" \
  > /dev/null 2> "$TMP/rvmin.err" || {
    cat "$TMP/rvmin.err" >&2
    fail "llvm-mc -triple=rvmin rejected the corpus"
  }

extract() {
  grep -o 'encoding: \[[^]]*\]' "$1" | sed 's/encoding: //'
}

"$BIN/llvm-mc" -triple=rvmin -show-encoding "$CORPUS" > "$TMP/rvmin.txt"
"$BIN/llvm-mc" -triple=riscv32 -show-encoding "$CORPUS" > "$TMP/ref.txt"
extract "$TMP/rvmin.txt" > "$TMP/rvmin.enc"
extract "$TMP/ref.txt" > "$TMP/ref.enc"

N=$(wc -l < "$TMP/rvmin.enc")
NREF=$(wc -l < "$TMP/ref.enc")
[[ "$N" -eq "$NREF" ]] || fail "encoder produced $N encodings, reference $NREF"
[[ "$N" -ge 60 ]] || fail "corpus only has $N instructions, want >= 60"

if ! diff -u "$TMP/ref.enc" "$TMP/rvmin.enc" > "$TMP/diff.txt"; then
  head -40 "$TMP/diff.txt" >&2
  fail "encoding mismatch between rvmin and riscv32"
fi

# 2. Object-level diff of .text (immediates only, no labels).
"$BIN/llvm-mc" -triple=rvmin -filetype=obj "$CORPUS" -o "$TMP/rvmin.o"
"$BIN/llvm-mc" -triple=riscv32 -filetype=obj "$CORPUS" -o "$TMP/ref.o"
"$BIN/llvm-objcopy" -O binary --only-section=.text "$TMP/rvmin.o" "$TMP/rvmin.bin"
"$BIN/llvm-objcopy" -O binary --only-section=.text "$TMP/ref.o" "$TMP/ref.bin"
cmp -s "$TMP/rvmin.bin" "$TMP/ref.bin" || fail ".text bytes differ from reference object"

# 2b. Fixup-level diff: branch/jump targets expressed as labels.
FIXUPS="$HERE/rvmin-fixup-corpus.s"
"$BIN/llvm-mc" -triple=rvmin -filetype=obj "$FIXUPS" -o "$TMP/rvmin-fix.o"
"$BIN/llvm-mc" -triple=riscv32 -filetype=obj "$FIXUPS" -o "$TMP/ref-fix.o"
"$BIN/llvm-objcopy" -O binary --only-section=.text "$TMP/rvmin-fix.o" "$TMP/rvmin-fix.bin"
"$BIN/llvm-objcopy" -O binary --only-section=.text "$TMP/ref-fix.o" "$TMP/ref-fix.bin"
cmp -s "$TMP/rvmin-fix.bin" "$TMP/ref-fix.bin" || fail ".text bytes differ for label fixups"

# 2c. Global-address operands (%hi/%lo).  Both assemblers leave the immediate
# fields zero and emit relocations, so .text must still match.
GLOBALS="$HERE/rvmin-global-corpus.s"
"$BIN/llvm-mc" -triple=rvmin -filetype=obj "$GLOBALS" -o "$TMP/rvmin-glob.o"
"$BIN/llvm-mc" -triple=riscv32 -filetype=obj "$GLOBALS" -o "$TMP/ref-glob.o"
"$BIN/llvm-objcopy" -O binary --only-section=.text "$TMP/rvmin-glob.o" "$TMP/rvmin-glob.bin"
"$BIN/llvm-objcopy" -O binary --only-section=.text "$TMP/ref-glob.o" "$TMP/ref-glob.bin"
cmp -s "$TMP/rvmin-glob.bin" "$TMP/ref-glob.bin" || fail ".text bytes differ for %hi/%lo operands"

RELOCS=$("$BIN/llvm-readelf" -r "$TMP/rvmin-glob.o")
for R in R_RISCV_HI20 R_RISCV_LO12_I R_RISCV_LO12_S; do
  [[ "$RELOCS" == *"$R"* ]] || fail "missing relocation $R in global corpus"
done

# 2d. The M extension (chapter 13).  These instructions only exist with
# `-mattr=+m`, so both assemblers get the feature and the negative case is
# checked too: without `+m` RVMin must reject the corpus with a diagnostic.
MCORPUS="$HERE/rvmin-m-encoding-corpus.s"
"$BIN/llvm-mc" -triple=rvmin -mattr=+m -show-encoding "$MCORPUS" > "$TMP/rvmin-m.txt"
"$BIN/llvm-mc" -triple=riscv32 -mattr=+m -show-encoding "$MCORPUS" > "$TMP/ref-m.txt"
extract "$TMP/rvmin-m.txt" > "$TMP/rvmin-m.enc"
extract "$TMP/ref-m.txt" > "$TMP/ref-m.enc"
NM=$(wc -l < "$TMP/rvmin-m.enc")
[[ "$NM" -eq 12 ]] || fail "M corpus produced $NM encodings, want 12"
diff -u "$TMP/ref-m.enc" "$TMP/rvmin-m.enc" > "$TMP/diff-m.txt" \
  || { head -20 "$TMP/diff-m.txt" >&2; fail "M encoding mismatch between rvmin and riscv32"; }

if "$BIN/llvm-mc" -triple=rvmin -filetype=obj "$MCORPUS" -o "$TMP/no-m.o" \
     > /dev/null 2> "$TMP/no-m.err"; then
  fail "llvm-mc -triple=rvmin accepted M instructions without +m"
fi
grep -q "requires the following: 'M'" "$TMP/no-m.err" \
  || fail "missing 'requires M' diagnostic without +m"

# 2e. Zbb (chapter 21).  The same shape as the M corpus: byte-identical
# encodings with -mattr=+zbb, and a diagnostic when the feature is missing.
ZB="$HERE/rvmin-zbb-encoding-corpus.s"
"$BIN/llvm-mc" -triple=rvmin -mattr=+zbb -show-encoding "$ZB" > "$TMP/rvmin-zb.txt"
"$BIN/llvm-mc" -triple=riscv32 -mattr=+zbb -show-encoding "$ZB" > "$TMP/ref-zb.txt"
extract "$TMP/rvmin-zb.txt" > "$TMP/rvmin-zb.enc"
extract "$TMP/ref-zb.txt" > "$TMP/ref-zb.enc"
NZB=$(wc -l < "$TMP/rvmin-zb.enc")
[[ "$NZB" -eq 19 ]] || fail "Zbb corpus produced $NZB encodings, want 19"
diff -u "$TMP/ref-zb.enc" "$TMP/rvmin-zb.enc" > "$TMP/diff-zb.txt" \
  || { head -20 "$TMP/diff-zb.txt" >&2; fail "Zbb encoding mismatch between rvmin and riscv32"; }

if "$BIN/llvm-mc" -triple=rvmin -filetype=obj "$ZB" -o "$TMP/no-zb.o" \
     > /dev/null 2> "$TMP/no-zb.err"; then
  fail "llvm-mc -triple=rvmin accepted Zbb instructions without +zbb"
fi
grep -q "'Zbb' (Basic Bit-Manipulation)" "$TMP/no-zb.err" \
  || fail "missing Zbb diagnostic without +zbb"

# 2f. Zbs.
ZS="$HERE/rvmin-zbs-encoding-corpus.s"
"$BIN/llvm-mc" -triple=rvmin -mattr=+zbs -show-encoding "$ZS" > "$TMP/rvmin-zs.txt"
"$BIN/llvm-mc" -triple=riscv32 -mattr=+zbs -show-encoding "$ZS" > "$TMP/ref-zs.txt"
extract "$TMP/rvmin-zs.txt" > "$TMP/rvmin-zs.enc"
extract "$TMP/ref-zs.txt" > "$TMP/ref-zs.enc"
NZS=$(wc -l < "$TMP/rvmin-zs.enc")
[[ "$NZS" -eq 8 ]] || fail "Zbs corpus produced $NZS encodings, want 8"
diff -u "$TMP/ref-zs.enc" "$TMP/rvmin-zs.enc" > "$TMP/diff-zs.txt" \
  || { head -20 "$TMP/diff-zs.txt" >&2; fail "Zbs encoding mismatch between rvmin and riscv32"; }

if "$BIN/llvm-mc" -triple=rvmin -filetype=obj "$ZS" -o "$TMP/no-zs.o" \
     > /dev/null 2> "$TMP/no-zs.err"; then
  fail "llvm-mc -triple=rvmin accepted Zbs instructions without +zbs"
fi
grep -q "'Zbs' (Single-Bit Instructions)" "$TMP/no-zs.err" \
  || fail "missing Zbs diagnostic without +zbs"

# 2g. A extension (chapter 22).  Same shape again: byte-identical encodings
# with -mattr=+a, and a diagnostic when the feature is missing.
AC="$HERE/rvmin-a-encoding-corpus.s"
"$BIN/llvm-mc" -triple=rvmin -mattr=+a -show-encoding "$AC" > "$TMP/rvmin-a.txt"
"$BIN/llvm-mc" -triple=riscv32 -mattr=+a -show-encoding "$AC" > "$TMP/ref-a.txt"
extract "$TMP/rvmin-a.txt" > "$TMP/rvmin-a.enc"
extract "$TMP/ref-a.txt" > "$TMP/ref-a.enc"
NA=$(wc -l < "$TMP/rvmin-a.enc")
[[ "$NA" -eq 20 ]] || fail "A corpus produced $NA encodings, want 20"
diff -u "$TMP/ref-a.enc" "$TMP/rvmin-a.enc" > "$TMP/diff-a.txt" \
  || { head -20 "$TMP/diff-a.txt" >&2; fail "A encoding mismatch between rvmin and riscv32"; }

if "$BIN/llvm-mc" -triple=rvmin -filetype=obj "$AC" -o "$TMP/no-a.o" \
     > /dev/null 2> "$TMP/no-a.err"; then
  fail "llvm-mc -triple=rvmin accepted A instructions without +a"
fi
grep -q "'A' (Atomic Instructions)" "$TMP/no-a.err" \
  || fail "missing A diagnostic without +a"

# 3. ELF identification.
MACHINE=$("$BIN/llvm-readelf" -h "$TMP/rvmin.o" | awk -F: '/Machine:/ {gsub(/^ +| +$/, "", $2); print $2}')
[[ "$MACHINE" == "RISC-V" ]] || fail "ELF machine is '$MACHINE', want RISC-V"

echo "PASS: $N RV32I + $NM M + $NZB Zbb + $NZS Zbs + $NA A instructions, .text identical"
echo "      (incl. label fixups and %hi/%lo), ELF machine RISC-V, missing-feature diagnostic OK"
