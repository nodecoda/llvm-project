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

# Group the bytes of a binary into 2-byte lines, the input format
# `llvm-mc -disassemble` expects for 16-bit (compressed) instructions.
bytes_to_hex2() {
  od -An -v -tx1 "$1" | tr -s ' ' '\n' | grep -v '^$' | sed 's/^/0x/' |
    paste -d' ' - -
}

# roundtrip_c <name> <expected-insns> <corpus> [extra llvm-mc args...]
#
# Same contract as roundtrip(), but the corpus is entirely 16-bit: two bytes
# per instruction, two bytes per disassembler input line.
roundtrip_c() {
  local name=$1 want=$2 corpus=$3
  shift 3
  local extra=("$@")

  "$BIN/llvm-mc" -triple=rvmin "${extra[@]}" -filetype=obj "$corpus" \
    -o "$TMP/$name.o" || fail "$name: llvm-mc rejected the corpus"
  "$BIN/llvm-objcopy" -O binary --only-section=.text "$TMP/$name.o" \
    "$TMP/$name.bin"

  local nbytes ninsns
  nbytes=$(wc -c < "$TMP/$name.bin")
  ninsns=$((nbytes / 2))
  [[ "$ninsns" -eq "$want" ]] || \
    fail "$name: .text has $ninsns compressed instructions, want $want"

  bytes_to_hex2 "$TMP/$name.bin" > "$TMP/$name.hex"
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

  echo "PASS: $name ($want compressed instructions, .text round-trips byte-for-byte)"
}

# roundtrip_mixed <name> <expected-insns> <corpus> [extra llvm-mc args...]
#
# A stream with both 16- and 32-bit instructions cannot be split into fixed
# groups, so this disassembles through llvm-objdump (which advances by each
# MCInstrDesc::Size), drops the address/byte columns, re-assembles the printed
# text and compares .text.  It is the end-to-end proof that the 16-bit decoder
# does not swallow the following word.
roundtrip_mixed() {
  local name=$1 want=$2 corpus=$3
  shift 3
  local extra=("$@")

  "$BIN/llvm-mc" -triple=rvmin "${extra[@]}" -filetype=obj "$corpus" \
    -o "$TMP/$name.o" || fail "$name: llvm-mc rejected the corpus"
  "$BIN/llvm-objcopy" -O binary --only-section=.text "$TMP/$name.o" \
    "$TMP/$name.bin"

  "$BIN/llvm-objdump" -d --no-show-raw-insn --triple=rvmin "${extra[@]}" \
    "$TMP/$name.o" > "$TMP/$name.dump"
  local dumped
  dumped=$(grep -cE '^[[:space:]]+[0-9a-f]+:' "$TMP/$name.dump" || true)
  [[ "$dumped" -eq "$want" ]] || \
    fail "$name: llvm-objdump decoded $dumped of $want instructions"
  grep -q '<unknown>' "$TMP/$name.dump" && \
    fail "$name: llvm-objdump hit an unknown encoding"

  sed -n 's/^[[:space:]]*[0-9a-f]*:[[:space:]]*//p' "$TMP/$name.dump" |
    sed 's/[[:space:]]*$//' > "$TMP/$name.s"
  "$BIN/llvm-mc" -triple=rvmin "${extra[@]}" -filetype=obj "$TMP/$name.s" \
    -o "$TMP/$name.rt.o" || fail "$name: could not re-assemble the output"
  "$BIN/llvm-objcopy" -O binary --only-section=.text "$TMP/$name.rt.o" \
    "$TMP/$name.rt.bin"
  cmp -s "$TMP/$name.bin" "$TMP/$name.rt.bin" || \
    fail "$name: mixed-width round trip differs from the original"

  echo "PASS: $name ($want instructions, mixed 16/32-bit .text round-trips)"
}

roundtrip rv32i 85 "$HERE/rvmin-encoding-corpus.s"
roundtrip m 12 "$HERE/rvmin-m-encoding-corpus.s" --mattr=+m
# Chapter 22: A instructions print `lr.w a0, (a1)`, i.e. the zero-displacement
# form the parser accepts, so this also proves the asm printer/parser symmetry.
# Chapter 46 removed fence.i from this corpus (it is Zifencei now), so A
# round-trips 19 instructions instead of 20.
roundtrip a 19 "$HERE/rvmin-a-encoding-corpus.s" --mattr=+a
# Chapter 40: F.  This also covers the explicit `rtz` spelling: the printer
# must reproduce it so the output can be assembled again into the same bytes.
roundtrip f 28 "$HERE/rvmin-f-encoding-corpus.s" --mattr=+f
# Chapter 41: D.  Same `rtz` symmetry; the double registers use the FPR64
# decoder hook but the same architectural table.
roundtrip d 28 "$HERE/rvmin-d-encoding-corpus.s" --mattr=+f,+d
# Chapter 46: Zicsr + Zifencei.  Aliases print back as aliases, so this also
# proves the InstAlias table is symmetric with the asm matcher.
roundtrip zicsr 24 "$HERE/rvmin-zicsr-encoding-corpus.s" --mattr=+zicsr,+zifencei,+f
# Chapter 47: V (Zve32x subset).  All instructions are 32-bit; this also proves
# that `printVTypeI` spells the token group back and that v0-v31 print as vN.
roundtrip v 41 "$HERE/rvmin-v-encoding-corpus.s" --mattr=+v,+f
# Chapter 43: C.  The corpus is 35 explicit compressed instructions (integer,
# F and D variants); the second run is a 16/32-bit mixed stream.
roundtrip_c c 35 "$HERE/rvmin-c-encoding-corpus.s" --mattr=+c,+f,+d
roundtrip_mixed mixed 6 "$HERE/rvmin-mixed-encoding-corpus.s" --mattr=+c

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

echo "PASS: 191 encoding-corpus instructions + 35 compressed + 6 mixed + 24 zicsr/zifencei + 6 label-resolved branches/jumps"
