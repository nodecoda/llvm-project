#!/usr/bin/env bash
#
# Chapter 12 gate: compile, assemble, link and RUN a freestanding RV32I
# program.
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
TRIPLE=riscv32-unknown-elf

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

# 1. C -> LLVM IR (clang, RV32I target) -> RV32I assembly (llc).
"$CC" --target="$TRIPLE" -march=rv32i -mabi=ilp32 -O2 -ffreestanding \
  -fno-builtin -S -emit-llvm "$RT/demo.c" -o "$TMP/demo.ll" \
  || fail "clang could not compile demo.c"
"$LLC" -march=rvmin -mtriple="$TRIPLE" -O2 "$TMP/demo.ll" -o "$TMP/demo.s" \
  || fail "llc could not select demo.c"

# 2. The program and the runtime are assembled by RVMin's own MC layer.
"$MC" -triple=rvmin -filetype=obj "$TMP/demo.s" -o "$TMP/demo.o" \
  || fail "llvm-mc rejected llc's output"
"$MC" -triple=rvmin -filetype=obj "$RT/rvmin-rt.S" -o "$TMP/rt.o" \
  || fail "llvm-mc rejected rvmin-rt.S"
"$MC" -triple=rvmin -filetype=obj "$RT/start.S" -o "$TMP/start.o" \
  || fail "llvm-mc rejected start.S"

# 3. Link into a static ET_EXEC that QEMU can load.
"$LD" -m elf32lriscv -T "$RT/link.ld" -o "$TMP/demo" \
  "$TMP/start.o" "$TMP/demo.o" "$TMP/rt.o" \
  || fail "ld.lld could not link the program"

# 4. Run it.
OUT=$("$QEMU" "$TMP/demo") || STATUS=$?
STATUS=${STATUS:-0}
echo "$OUT"
[[ "$STATUS" -eq 0 ]] || fail "demo exited with $STATUS"
echo "PASS: linked $(stat -c%s "$TMP/demo") bytes, ran under $QEMU, all checks green"
