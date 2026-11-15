#!/usr/bin/env bash
#
# Chapter 12 gate: compile, assemble, link and RUN a freestanding RV32I
# program.  Chapter 13 runs the same program twice, once as plain RV32I (with
# the runtime's libcalls) and once with the M extension, and requires identical
# output.  Chapter 16 adds a second program whose stack frames are larger than
# 2 KiB, compiled at -O0 and -O2.
#
# Usage: rvmin-run.sh <llvm-bin-dir> [qemu]
#
# The interesting part is that every step uses RVMin's own tools: clang emits
# IR, llc emits RV32I assembly, and llvm-mc -triple=rvmin assembles it (so the
# program also proves the assembler accepts real compiler output).  Only the
# linker and the emulator are external.
#
set -euo pipefail

BIN=${1:?usage: rvmin-run.sh <llvm-bin-dir> [qemu]}
QEMU=${2:-qemu-riscv32-static}
LD=${LD:-ld.lld}
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
RT="$HERE/rt"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

CC="$BIN/clang"
LLC="$BIN/llc"
MC="$BIN/llvm-mc"
READELF="$BIN/llvm-readelf"
TRIPLE=riscv32-unknown-elf

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

# Build demo.c for one feature set and run it under QEMU.  Prints the
# program's stdout; the return value is the program's exit status.
run_variant() {
  local suffix=$1 mattr=$2
  local LLC_ARGS=(-march=rvmin -mtriple="$TRIPLE" -O2)
  local MC_ARGS=(-triple=rvmin)
  if [[ -n "$mattr" ]]; then
    LLC_ARGS+=(-mattr="$mattr")
    MC_ARGS+=(-mattr="$mattr")
  fi

  # 1. C -> LLVM IR (clang, RV32I target) -> assembly (llc).
  "$CC" --target="$TRIPLE" -march=rv32i -mabi=ilp32 -O2 -ffreestanding \
    -fno-builtin -S -emit-llvm "$RT/demo.c" -o "$TMP/demo$suffix.ll" \
    || fail "clang could not compile demo.c (${mattr:-rv32i})"
  "$LLC" "${LLC_ARGS[@]}" "$TMP/demo$suffix.ll" -o "$TMP/demo$suffix.s" \
    || fail "llc could not select demo.c (${mattr:-rv32i})"

  # 2. The program and the runtime are assembled by RVMin's own MC layer.
  "$MC" "${MC_ARGS[@]}" -filetype=obj "$TMP/demo$suffix.s" \
    -o "$TMP/demo$suffix.o" || fail "llvm-mc rejected llc's output"
  "$MC" "${MC_ARGS[@]}" -filetype=obj "$RT/rvmin-rt.S" -o "$TMP/rt$suffix.o" \
    || fail "llvm-mc rejected rvmin-rt.S"
  "$MC" "${MC_ARGS[@]}" -filetype=obj "$RT/start.S" -o "$TMP/start$suffix.o" \
    || fail "llvm-mc rejected start.S"

  # 3. Link into a static ET_EXEC that QEMU can load.
  "$LD" -m elf32lriscv -T "$RT/link.ld" -o "$TMP/demo$suffix" \
    "$TMP/start$suffix.o" "$TMP/demo$suffix.o" "$TMP/rt$suffix.o" \
    || fail "ld.lld could not link the program (${mattr:-rv32i})"

  # 4. Run it.
  "$QEMU" "$TMP/demo$suffix"
}

# RV32I: multiply/divide/remainder go through rvmin-rt.S.
STATUS1=0
OUT1=$(run_variant "" "") || STATUS1=$?
[[ "$STATUS1" -eq 0 ]] || fail "RV32I demo exited with $STATUS1"
echo "rv32i: $OUT1"

# RV32I + M: the same program with single-instruction mul/div/rem.
STATUS2=0
OUT2=$(run_variant "-m" "+m") || STATUS2=$?
[[ "$STATUS2" -eq 0 ]] || fail "RV32I+M demo exited with $STATUS2"
echo "rv32i+m: $OUT2"
[[ "$OUT1" == "$OUT2" ]] || fail "the two feature sets disagree: '$OUT1' vs '$OUT2'"

# The M build must not reference the multiply/divide libcalls any more.  (The
# 64-bit shifts still do: `<<` and `>>` on `long long` are not part of M.)
RELOCS=$("$READELF" -r "$TMP/demo-m.o")
for SYM in __mulsi3 __divsi3 __udivsi3 __modsi3 __umodsi3; do
  [[ "$RELOCS" != *"$SYM"* ]] || fail "+m build still references $SYM"
done
grep -q '__ashldi3' <<< "$RELOCS" || fail "+m build lost the 64-bit shift libcall"

# Chapter 16 gate: frames larger than 2 KiB.  `-O0` and `-O2` exercise both
# register allocators; `-verify-machineinstrs` checks the scavenged scratch
# registers that eliminateFrameIndex creates.
run_large_frame() {
  local O=$1
  "$CC" --target="$TRIPLE" -march=rv32i -mabi=ilp32 -O"$O" -ffreestanding \
    -fno-builtin -S -emit-llvm "$RT/large-frame.c" -o "$TMP/lf$O.ll" \
    || fail "clang could not compile large-frame.c (-O$O)"
  "$LLC" -march=rvmin -mtriple="$TRIPLE" -O"$O" -verify-machineinstrs \
    "$TMP/lf$O.ll" -o "$TMP/lf$O.s" \
    || fail "llc could not select large-frame.c (-O$O)"
  "$MC" -triple=rvmin -filetype=obj "$TMP/lf$O.s" -o "$TMP/lf$O.o" \
    || fail "llvm-mc rejected the large-frame output (-O$O)"
  "$MC" -triple=rvmin -filetype=obj "$RT/rvmin-rt.S" -o "$TMP/rt-lf$O.o" \
    || fail "llvm-mc rejected rvmin-rt.S"
  "$MC" -triple=rvmin -filetype=obj "$RT/start.S" -o "$TMP/start-lf$O.o" \
    || fail "llvm-mc rejected start.S"
  "$LD" -m elf32lriscv -T "$RT/link.ld" -o "$TMP/lf$O" \
    "$TMP/start-lf$O.o" "$TMP/lf$O.o" "$TMP/rt-lf$O.o" \
    || fail "ld.lld could not link the large-frame program (-O$O)"
  "$QEMU" "$TMP/lf$O"
}

for O in 0 2; do
  STATUS=0
  run_large_frame "$O" || STATUS=$?
  [[ "$STATUS" -eq 0 ]] || fail "large-frame -O$O exited with $STATUS"
  grep -q 'lui' "$TMP/lf$O.s" || fail "large-frame -O$O never materialized an address"
done
echo "PASS: 3 KiB frames (and 9 nested ones) ran under $QEMU at -O0 and -O2"

SIZE1=$(stat -c%s "$TMP/demo")
SIZE2=$(stat -c%s "$TMP/demo-m")
echo "PASS: linked $SIZE1 bytes (rv32i) and $SIZE2 bytes (rv32i+m), both ran under $QEMU"
echo "      with identical output; +m dropped the mul/div/rem libcalls entirely"
