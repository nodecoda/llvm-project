#!/usr/bin/env bash
#
# Chapter 50 gate: the Zve32x teaching subset, end to end.
#
# Usage: rvmin-e2e-v.sh <llvm-bin-dir>
#
# For each optimisation level (O0..O3) this checks:
#   1. clang can compile scripts/e2e-v/v-demo.c to IR with the V feature set;
#   2. llc selects vector instructions and emits an object whose machine IR
#      passes -verify-machineinstrs;
#   3. llc's assembly is re-assembled byte-for-byte by both RVMin's MC layer
#      and the reference riscv32 +v assembler;
#   4. the expected paths are present: .vv, .vi/.vx and the reduction.
#
# The program is deliberately outside scripts/e2e/*.c: the 288-object matrix
# and the 432-object clang matrix are chapter 45 baselines and must not grow.
#
set -euo pipefail

BIN=${1:?usage: rvmin-e2e-v.sh <llvm-bin-dir>}
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
SRC="$HERE/e2e-v/v-demo.c"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

CC="$BIN/clang"
LLC="$BIN/llc"
MC="$BIN/llvm-mc"
OBJCOPY="$BIN/llvm-objcopy"
READELF="$BIN/llvm-readelf"
TRIPLE=riscv32-unknown-elf
MATTR=+f,+d,+v

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

OPTS="0 1 2 3"
OBJECTS=0

for O in $OPTS; do
  base="$TMP/v-demo.O$O"

  # 1. C -> IR.  ilp32d keeps the module flag consistent with +f,+d; the
  # vector instructions themselves come from +v.
  "$CC" --target="$TRIPLE" -march=rv32imafdv -mabi=ilp32d -O"$O" \
    -ffreestanding -fno-builtin -S -emit-llvm "$SRC" -o "$base.ll" \
    || fail "clang could not compile v-demo.c at -O$O"

  # 2. IR -> object, with machine IR verified.
  "$LLC" -march=rvmin -mtriple="$TRIPLE" -O"$O" -mattr="$MATTR" \
    -verify-machineinstrs "$base.ll" -filetype=obj -o "$base.o" \
    || fail "llc -filetype=obj failed at -O$O"

  MACHINE=$("$READELF" -h "$base.o" | awk -F: '/Machine:/ {gsub(/^ +| +$/, "", $2); print $2}')
  [[ "$MACHINE" == "RISC-V" ]] || fail "-O$O: ELF machine is '$MACHINE'"

  # 3. Assembly round trip against both assemblers.
  "$LLC" -march=rvmin -mtriple="$TRIPLE" -O"$O" -mattr="$MATTR" \
    "$base.ll" -o "$base.s" || fail "llc -filetype=asm failed at -O$O"

  if grep -nE '^\s*(Pseudo|CALL|RET|ADJCALLSTACK)' "$base.s" >/dev/null; then
    fail "-O$O: pseudo instruction leaked into the assembly"
  fi

  "$OBJCOPY" -O binary --only-section=.text "$base.o" "$base.o.text"
  for ASM in rvmin riscv32; do
    "$MC" -triple="$ASM" -mattr="$MATTR" -filetype=obj "$base.s" \
      -o "$base.$ASM.o" || fail "-O$O: $ASM could not re-assemble llc's output"
    "$OBJCOPY" -O binary --only-section=.text "$base.$ASM.o" "$base.$ASM.text"
    cmp -s "$base.o.text" "$base.$ASM.text" \
      || fail "-O$O: .text differs between llc and $ASM"
  done

  # 4. Path coverage.  O0 keeps constants as full vectors; from O1 the
  # selector folds them into .vi/.vx, so those two are checked from O1 on.
  grep -q 'vsetivli' "$base.s" || fail "-O$O: no vsetivli"
  grep -q 'vadd\.vv' "$base.s" || fail "-O$O: no vadd.vv"
  grep -q 'vredsum\.vs' "$base.s" || fail "-O$O: no vredsum.vs"
  if [[ "$O" != "0" ]]; then
    grep -q 'vadd\.vi' "$base.s" || fail "-O$O: no vadd.vi"
    grep -q 'vadd\.vx' "$base.s" || fail "-O$O: no vadd.vx"
  fi

  OBJECTS=$((OBJECTS + 1))
done

echo "PASS: V 1 program x 4 optimization levels = $OBJECTS objects"
echo "      every .text identical to its rvmin and riscv32 reassembly;"
echo "      .vv/.vi/.vx/reduction all selected"
