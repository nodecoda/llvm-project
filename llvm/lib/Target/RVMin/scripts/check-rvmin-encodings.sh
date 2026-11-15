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

# 3. ELF identification.
MACHINE=$("$BIN/llvm-readelf" -h "$TMP/rvmin.o" | awk -F: '/Machine:/ {gsub(/^ +| +$/, "", $2); print $2}')
[[ "$MACHINE" == "RISC-V" ]] || fail "ELF machine is '$MACHINE', want RISC-V"

echo "PASS: $N instructions, .text identical (incl. label fixups), ELF machine RISC-V"
