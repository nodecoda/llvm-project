#!/usr/bin/env bash
#
# Chapter 20 gate: the disassembler is the inverse of the MC layer.
#
# Usage: check-rvmin-disasm.sh <llvm-bin-dir>
#
# For every instruction in the RV32I and M encoding corpora this script does:
#
#   asm --llvm-mc -filetype=obj--> obj --llvm-objcopy--> .text bytes
#       --od/llvm-mc -disassemble--> asm --llvm-mc -filetype=obj--> obj
#
# and requires the two .text sections to be byte-identical.  Byte equality is
# stronger than text equality: it also proves that the printer's immediates
# (including negative branch/jump offsets) re-assemble to the same encoding.
#
# Two extra checks guard against a disassembler that only agrees with itself:
#   * llvm-objdump --triple=rvmin must decode every word (no unknowns);
#   * the fixup corpus (labels, forward/backward branches, large offsets) is
#     disassembled from its *resolved* bytes and assembled back.
#
set -euo pipefail

BIN=${1:?usage: check-rvmin-disasm.sh <llvm-bin-dir>}
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

# Group the bytes of a binary into 4-byte lines of 0xNN tokens, the input
# format `llvm-mc -disassemble` expects.
bytes_to_hex() {
  od -An -v -tx1 "$1" | tr -s ' ' '\n' | grep -v '^$' | sed 's/^/0x/' |
    paste -d' ' - - - -
}

# roundtrip <name> <expected-insns> <corpus> [extra llvm-mc args...]
roundtrip() {
  local name=$1 want=$2 corpus=$3
  shift 3
  local extra=("$@")

  "$BIN/llvm-mc" -triple=rvmin "${extra[@]}" -filetype=obj "$corpus" \
    -o "$TMP/$name.o" || fail "$name: llvm-mc rejected the corpus"
  "$BIN/llvm-objcopy" -O binary --only-section=.text "$TMP/$name.o" \
    "$TMP/$name.bin"

  local nbytes ninsns
  nbytes=$(wc -c < "$TMP/$name.bin")
  ninsns=$((nbytes / 4))
  [[ "$ninsns" -eq "$want" ]] || \
    fail "$name: .text has $ninsns instructions, want $want"

  bytes_to_hex "$TMP/$name.bin" > "$TMP/$name.hex"
  "$BIN/llvm-mc" -triple=rvmin "${extra[@]}" -disassemble \
    < "$TMP/$name.hex" > "$TMP/$name.s" 2> "$TMP/$name.err" || \
    { cat "$TMP/$name.err" >&2; fail "$name: llvm-mc -disassemble failed"; }
  [[ ! -s "$TMP/$name.err" ]] || \
    { cat "$TMP/$name.err" >&2; fail "$name: disassembler left warnings"; }

  local printed
  printed=$(grep -c $'^\t' "$TMP/$name.s" || true)
  [[ "$printed" -eq "$want" ]] || \
    fail "$name: disassembler printed $printed instructions, want $want"

  "$BIN/llvm-mc" -triple=rvmin "${extra[@]}" -filetype=obj "$TMP/$name.s" \
    -o "$TMP/$name.rt.o" || fail "$name: could not re-assemble the output"
  "$BIN/llvm-objcopy" -O binary --only-section=.text "$TMP/$name.rt.o" \
    "$TMP/$name.rt.bin"
  cmp -s "$TMP/$name.bin" "$TMP/$name.rt.bin" || \
    fail "$name: round-tripped .text differs from the original"

  # Independent decoder: llvm-objdump uses the same table but a different
  # driver, so this catches printing/registration mistakes in llvm-mc only.
  "$BIN/llvm-objdump" --triple=rvmin "${extra[@]}" -d "$TMP/$name.o" \
    > "$TMP/$name.dump"
  local dumped
  dumped=$(grep -cE '^[[:space:]]+[0-9a-f]+:' "$TMP/$name.dump" || true)
  [[ "$dumped" -eq "$want" ]] || \
    fail "$name: llvm-objdump decoded $dumped of $want instructions"
  if grep -q '<unknown>' "$TMP/$name.dump"; then
    fail "$name: llvm-objdump hit an unknown encoding"
  fi

  echo "PASS: $name ($want instructions, .text round-trips byte-for-byte)"
}

roundtrip rv32i 85 "$HERE/rvmin-encoding-corpus.s"
roundtrip m 12 "$HERE/rvmin-m-encoding-corpus.s" --mattr=+m

# Label-resolved branches: the assembler has already turned the labels into
# byte offsets, so the disassembler sees the same simm13/simm21 fields as for
# the numeric cases above, but with realistic forward/backward values.
cat > "$TMP/labels.s" <<'ASM'
        .text
start:
        beq     a0, a1, fwd
        bne     a0, a1, back
        jal     zero, fwd
back:
        addi    a0, a0, 1
fwd:
        jal     ra, start
        bltu    a1, a2, back
ASM
roundtrip labels 6 "$TMP/labels.s"

echo "PASS: 97 encoding-corpus instructions + 6 label-resolved branches/jumps"
