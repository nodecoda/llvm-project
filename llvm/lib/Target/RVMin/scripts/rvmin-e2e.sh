#!/usr/bin/env bash
#
# Chapter 11 gate: clang -> LLVM IR -> llc -> object, for a small C corpus, at
# every optimisation level, with a byte-exact assembler round trip.
#
# Chapter 13 extends the sweep with `-mattr=+m`: the same programs must compile
# and round-trip when multiply/divide/remainder become single instructions.
# Chapter 40 adds `-mattr=+f`; that cell compiles its IR with -march=rv32if so
# the module's ABI flag (ilp32f) agrees with the backend's feature set.
#
# Usage: rvmin-e2e.sh <llvm-bin-dir>
#
# For each program and each -O level this checks four independent things:
#   1. clang can produce IR for the RV32I target and llc accepts it
#      (with -verify-machineinstrs, so the machine IR is validated);
#   2. llc emits an object file whose ELF machine is RISC-V;
#   3. llc's assembly, re-assembled by llvm-mc, gives a byte-identical .text;
#   4. no target pseudo instruction leaked into the assembly (the listing must
#      be plain RV32I that any external assembler could have produced).
#
set -euo pipefail

BIN=${1:?usage: rvmin-e2e.sh <llvm-bin-dir>}
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
SRC="$HERE/e2e"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

CC="$BIN/clang"
LLC="$BIN/llc"
MC="$BIN/llvm-mc"
OBJCOPY="$BIN/llvm-objcopy"
READELF="$BIN/llvm-readelf"

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

TRIPLE=riscv32-unknown-elf
OPTS="0 1 2 3"
MATTRS=("" "+m" "+f")
PROGRAMS=0
CASES=0
TOTAL_TEXT=0

for c in "$SRC"/*.c; do
  name=$(basename "$c" .c)
  PROGRAMS=$((PROGRAMS + 1))
  for O in $OPTS; do
    for MATTR in "${MATTRS[@]}"; do
      tag="$name.O$O${MATTR:++${MATTR#+}}"
      base="$TMP/$tag"
      LLC_ARGS=(-march=rvmin -mtriple="$TRIPLE" -O"$O")
      [[ -n "$MATTR" ]] && LLC_ARGS+=(-mattr="$MATTR")

      # The feature set decides both the backend flags and the frontend's
      # ABI: an F-enabled object must carry "ilp32f" in its IR, otherwise the
      # backend's consistency check is supposed to reject it.
      CARCH=rv32i
      CABI=ilp32
      if [[ "$MATTR" == "+f" ]]; then
        CARCH=rv32if
        CABI=ilp32f
      fi
      "$CC" --target="$TRIPLE" -march="$CARCH" -mabi="$CABI" -O"$O" \
        -S -emit-llvm "$c" -o "$base.ll" \
        || fail "$tag: clang failed"

      # 1 + 2: object file, machine IR verified.
      "$LLC" "${LLC_ARGS[@]}" -verify-machineinstrs \
        "$base.ll" -filetype=obj -o "$base.o" \
        || fail "$tag: llc -filetype=obj failed"

      MACHINE=$("$READELF" -h "$base.o" | awk -F: '/Machine:/ {gsub(/^ +| +$/, "", $2); print $2}')
      [[ "$MACHINE" == "RISC-V" ]] || fail "$tag: ELF machine is '$MACHINE'"

      # 3: assembler round trip.
      "$LLC" "${LLC_ARGS[@]}" "$base.ll" -o "$base.s" \
        || fail "$tag: llc -filetype=asm failed"
      if grep -nE '^\s*(Pseudo|CALL|RET|ADJCALLSTACK)' "$base.s" >/dev/null; then
        fail "$tag: pseudo instruction leaked into the assembly"
      fi
      # The reference assembler needs the same feature string: default
      # `riscv32` is plain RV32I, so `+m` output would otherwise be rejected.
      MC_ARGS=(-triple="$TRIPLE")
      [[ -n "$MATTR" ]] && MC_ARGS+=(-mattr="$MATTR")
      "$MC" "${MC_ARGS[@]}" -filetype=obj "$base.s" -o "$base.mc.o" \
        || fail "$tag: llvm-mc could not re-assemble the output"

      for f in "$base.o" "$base.mc.o"; do
        "$OBJCOPY" -O binary --only-section=.text "$f" "$f.text"
      done
      cmp -s "$base.o.text" "$base.mc.o.text" \
        || fail "$tag: .text differs between llc and llvm-mc"

      # 4: the listing must be plain RV32I (plus M when requested).
      SIZE=$(stat -c%s "$base.o.text")
      [[ "$SIZE" -gt 0 ]] || fail "$tag: empty .text"
      TOTAL_TEXT=$((TOTAL_TEXT + SIZE))
      CASES=$((CASES + 1))
    done
  done
done

echo "PASS: $PROGRAMS programs x $OPTS optimization levels x ${#MATTRS[@]} feature sets"
echo "      = $CASES objects, every .text identical to its llvm-mc reassembly"
echo "      ($TOTAL_TEXT bytes total)"
