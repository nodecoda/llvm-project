#!/usr/bin/env bash
#
# Chapter 12 gate: compile, assemble, link and RUN a freestanding RV32I
# program.  Chapter 13 runs the same program twice, once as plain RV32I (with
# the runtime's libcalls) and once with the M extension, and requires identical
# output.  Chapter 16 adds a second program whose stack frames are larger than
# 2 KiB, compiled at -O0 and -O2.  Chapter 17 adds a third one that recurses a
# million levels deep and only fits in the default stack because the recursive
# calls became jumps.  Chapter 18 adds a fourth: switch statements whose dense
# cases become a jump table in .rodata.  Chapter 19 adds a fifth: constant
# multiplies and divisions, run both as RV32I and with M.  Chapter 42 adds
# the F/D programs: the float checks run with F alone and under D, the double
# checks need D (the soft-float double libcalls are not in rvmin-rt.S).
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

# Chapter 17 gate: one million tail calls.  Each function has a 16-byte frame,
# so without tail call optimization the recursion would need 16 MiB of stack
# (the program gets 8 MiB) and die with SIGSEGV -- -O0 still does exactly that,
# which is what makes this check meaningful.
run_tail_calls() {
  "$CC" --target="$TRIPLE" -march=rv32i -mabi=ilp32 -O2 -ffreestanding \
    -fno-builtin -S -emit-llvm "$RT/tail.c" -o "$TMP/tail.ll" \
    || fail "clang could not compile tail.c"
  "$LLC" -march=rvmin -mtriple="$TRIPLE" -O2 -verify-machineinstrs \
    "$TMP/tail.ll" -o "$TMP/tail.s" \
    || fail "llc could not select tail.c"
  grep -q 'jalr zero, 0(t0)' "$TMP/tail.s" \
    || fail "tail.c emitted no tail call (expected jalr zero, 0(t0))"
  "$MC" -triple=rvmin -filetype=obj "$TMP/tail.s" -o "$TMP/tail.o" \
    || fail "llvm-mc rejected the tail-call output"
  "$MC" -triple=rvmin -filetype=obj "$RT/rvmin-rt.S" -o "$TMP/rt-tail.o" \
    || fail "llvm-mc rejected rvmin-rt.S"
  "$MC" -triple=rvmin -filetype=obj "$RT/start.S" -o "$TMP/start-tail.o" \
    || fail "llvm-mc rejected start.S"
  "$LD" -m elf32lriscv -T "$RT/link.ld" -o "$TMP/tail" \
    "$TMP/start-tail.o" "$TMP/tail.o" "$TMP/rt-tail.o" \
    || fail "ld.lld could not link the tail-call program"
  "$QEMU" "$TMP/tail"
}

STATUS=0
run_tail_calls || STATUS=$?
[[ "$STATUS" -eq 0 ]] || fail "tail-call program exited with $STATUS"
echo "PASS: 1e6 tail calls ran under $QEMU (16 MiB of frames would not fit)"

# Chapter 18 gate: a dense switch becomes a jump table, a sparse one stays a
# comparison chain, and both have to produce the answers encoded in the C file.
run_switch() {
  "$CC" --target="$TRIPLE" -march=rv32i -mabi=ilp32 -O2 -ffreestanding \
    -fno-builtin -S -emit-llvm "$RT/switch.c" -o "$TMP/switch.ll" \
    || fail "clang could not compile switch.c"
  "$LLC" -march=rvmin -mtriple="$TRIPLE" -O2 -verify-machineinstrs \
    "$TMP/switch.ll" -o "$TMP/switch.s" \
    || fail "llc could not select switch.c"
  grep -q 'LJTI' "$TMP/switch.s" || fail "switch.c emitted no jump table"
  grep -q 'jalr zero, 0(' "$TMP/switch.s" \
    || fail "switch.c emitted no indirect jump"
  "$MC" -triple=rvmin -filetype=obj "$TMP/switch.s" -o "$TMP/switch.o" \
    || fail "llvm-mc rejected the jump-table output"
  "$MC" -triple=rvmin -filetype=obj "$RT/rvmin-rt.S" -o "$TMP/rt-switch.o" \
    || fail "llvm-mc rejected rvmin-rt.S"
  "$MC" -triple=rvmin -filetype=obj "$RT/start.S" -o "$TMP/start-switch.o" \
    || fail "llvm-mc rejected start.S"
  "$LD" -m elf32lriscv -T "$RT/link.ld" -o "$TMP/switch" \
    "$TMP/start-switch.o" "$TMP/switch.o" "$TMP/rt-switch.o" \
    || fail "ld.lld could not link the switch program"
  "$QEMU" "$TMP/switch"
}

STATUS=0
run_switch || STATUS=$?
[[ "$STATUS" -eq 0 ]] || fail "switch program exited with $STATUS"
echo "PASS: jump-table switch program ran under $QEMU"

# Chapter 19 gate: constant multiply/divide.  Without M the constants in
# muldiv.c must not reach __mulsi3 (they are shifts and adds); with M the
# division by 7/10 must not reach __divsi3/__modsi3 but use the magic-number
# multiply instead.  Both builds must pass the same literal checks.
run_muldiv() {
  local suffix=$1 mattr=$2
  local LLC_ARGS=(-march=rvmin -mtriple="$TRIPLE" -O2 -verify-machineinstrs)
  local MC_ARGS=(-triple=rvmin)
  if [[ -n "$mattr" ]]; then
    LLC_ARGS+=(-mattr="$mattr")
    MC_ARGS+=(-mattr="$mattr")
  fi

  "$CC" --target="$TRIPLE" -march=rv32i -mabi=ilp32 -O2 -ffreestanding \
    -fno-builtin -S -emit-llvm "$RT/muldiv.c" -o "$TMP/md$suffix.ll" \
    || fail "clang could not compile muldiv.c ($suffix)"
  "$LLC" "${LLC_ARGS[@]}" "$TMP/md$suffix.ll" -o "$TMP/md$suffix.s" \
    || fail "llc could not select muldiv.c ($suffix)"

  if grep -q '__mulsi3' "$TMP/md$suffix.s"; then
    fail "muldiv.c ($suffix) still calls __mulsi3"
  fi
  if [[ -n "$mattr" ]]; then
    if grep -q '__divsi3\|__modsi3' "$TMP/md$suffix.s"; then
      fail "muldiv.c (+m) still calls the division runtime"
    fi
    grep -q 'mulh' "$TMP/md$suffix.s" \
      || fail "muldiv.c (+m) used no magic multiply"
  fi

  "$MC" "${MC_ARGS[@]}" -filetype=obj "$TMP/md$suffix.s" -o "$TMP/md$suffix.o" \
    || fail "llvm-mc rejected the muldiv output ($suffix)"
  "$MC" "${MC_ARGS[@]}" -filetype=obj "$RT/rvmin-rt.S" -o "$TMP/rt-md$suffix.o" \
    || fail "llvm-mc rejected rvmin-rt.S"
  "$MC" "${MC_ARGS[@]}" -filetype=obj "$RT/start.S" -o "$TMP/start-md$suffix.o" \
    || fail "llvm-mc rejected start.S"
  "$LD" -m elf32lriscv -T "$RT/link.ld" -o "$TMP/md$suffix" \
    "$TMP/start-md$suffix.o" "$TMP/md$suffix.o" "$TMP/rt-md$suffix.o" \
    || fail "ld.lld could not link the muldiv program ($suffix)"
  "$QEMU" "$TMP/md$suffix"
}

for variant in "rv32i:" "m:+m"; do
  SUFFIX=${variant%%:*}
  MATTR=${variant#*:}
  STATUS=0
  OUT=$(run_muldiv "$SUFFIX" "$MATTR") || STATUS=$?
  [[ "$STATUS" -eq 0 ]] || fail "muldiv ($SUFFIX) exited with $STATUS"
  echo "muldiv $SUFFIX: $OUT"
done
echo "PASS: constant multiply/divide program ran under $QEMU with and without M"

# Chapter 22 gate: atomics under QEMU.  QEMU's default rv32 CPU implements A,
# so the lr.w/sc.w instructions execute for real.  The static greps make the
# gate meaningful even if a future QEMU loses A: amoadd proves the AMO path,
# lr/sc prove the cmpxchg/LL-SC path.
run_atomics() {
  "$CC" --target="$TRIPLE" -march=rv32ia -mabi=ilp32 -O2 -ffreestanding \
    -fno-builtin -S -emit-llvm "$RT/atomics.c" -o "$TMP/atomics.ll" \
    || fail "clang could not compile atomics.c"
  "$LLC" -march=rvmin -mtriple="$TRIPLE" -mattr=+a -O2 -verify-machineinstrs \
    "$TMP/atomics.ll" -o "$TMP/atomics.s" \
    || fail "llc could not select atomics.c"
  grep -q 'amoadd.w.aqrl' "$TMP/atomics.s" \
    || fail "atomics.c emitted no AMO for fetch_add"
  grep -q 'lr.w.aqrl' "$TMP/atomics.s" \
    || fail "atomics.c emitted no lr.w"
  grep -q 'sc.w.aqrl' "$TMP/atomics.s" \
    || fail "atomics.c emitted no sc.w"
  grep -q '^[[:space:]]*fence$' "$TMP/atomics.s" \
    || fail "atomics.c emitted no fence"
  "$MC" -triple=rvmin -mattr=+a -filetype=obj "$TMP/atomics.s" \
    -o "$TMP/atomics.o" || fail "llvm-mc rejected the atomic output"
  "$MC" -triple=rvmin -mattr=+a -filetype=obj "$RT/rvmin-rt.S" \
    -o "$TMP/rt-at.o" || fail "llvm-mc rejected rvmin-rt.S"
  "$MC" -triple=rvmin -mattr=+a -filetype=obj "$RT/start.S" \
    -o "$TMP/start-at.o" || fail "llvm-mc rejected start.S"
  "$LD" -m elf32lriscv -T "$RT/link.ld" -o "$TMP/atomics" \
    "$TMP/start-at.o" "$TMP/atomics.o" "$TMP/rt-at.o" \
    || fail "ld.lld could not link the atomics program"
  "$QEMU" "$TMP/atomics"
}

STATUS=0
OUT=$(run_atomics) || STATUS=$?
[[ "$STATUS" -eq 0 ]] || fail "atomics program exited with $STATUS"
echo "atomics: $OUT"
echo "PASS: A-extension atomics ran under $QEMU (amoadd + lr.w/sc.w loops)"

SIZE1=$(stat -c%s "$TMP/demo")
SIZE2=$(stat -c%s "$TMP/demo-m")
echo "PASS: linked $SIZE1 bytes (rv32i) and $SIZE2 bytes (rv32i+m), both ran under $QEMU"
echo "      with identical output; +m dropped the mul/div/rem libcalls entirely"

# Chapter 42 gate: F/D code runs for real under QEMU.  The program exit code
# is the first failing check, so a broken conversion, comparison or calling
# convention is visible without any libc printing support.
run_fp() {
  local src=$1 suffix=$2 carch=$3 cabi=$4 mattr=$5
  local LLC_ARGS=(-march=rvmin -mtriple="$TRIPLE" -O2 -verify-machineinstrs
                  -mattr="$mattr")
  local MC_ARGS=(-triple=rvmin -mattr="$mattr")

  "$CC" --target="$TRIPLE" -march="$carch" -mabi="$cabi" -O2 -ffreestanding \
    -fno-builtin -S -emit-llvm "$RT/$src.c" -o "$TMP/$suffix.ll" \
    || fail "clang could not compile $src.c ($carch/$cabi)"
  "$LLC" "${LLC_ARGS[@]}" "$TMP/$suffix.ll" -o "$TMP/$suffix.s" \
    || fail "llc could not select $src.c ($carch/$cabi)"
  "$MC" "${MC_ARGS[@]}" -filetype=obj "$TMP/$suffix.s" -o "$TMP/$suffix.o" \
    || fail "llvm-mc rejected the $src output ($carch/$cabi)"
  "$MC" "${MC_ARGS[@]}" -filetype=obj "$RT/rvmin-rt.S" -o "$TMP/rt-$suffix.o" \
    || fail "llvm-mc rejected rvmin-rt.S"
  "$MC" "${MC_ARGS[@]}" -filetype=obj "$RT/start.S" -o "$TMP/start-$suffix.o" \
    || fail "llvm-mc rejected start.S"
  "$LD" -m elf32lriscv -T "$RT/link.ld" -o "$TMP/$suffix" \
    "$TMP/start-$suffix.o" "$TMP/$suffix.o" "$TMP/rt-$suffix.o" \
    || fail "ld.lld could not link $src ($carch/$cabi)"
  "$QEMU" "$TMP/$suffix"
}

# F alone: floats in FPR32, no double instructions at all.
STATUS=0
run_fp float f32-if rv32if ilp32f +f || STATUS=$?
[[ "$STATUS" -eq 0 ]] || fail "float program (rv32if/ilp32f) exited with $STATUS"
grep -q 'fadd.s' "$TMP/f32-if.s" || fail "the F build emitted no fadd.s"
grep -q 'fcvt.w.s' "$TMP/f32-if.s" || fail "the F build emitted no fcvt.w.s"
if grep -q 'fadd.d\|fld\|fsd' "$TMP/f32-if.s"; then
  fail "the F-only build already used D instructions"
fi
RELOCS=$("$READELF" -r "$TMP/f32-if.o")
for SYM in __addsf3 __mulsf3 __eqsf2 __ltsf2 __fixsfsi __floatsisf; do
  [[ "$RELOCS" != *"$SYM"* ]] || fail "the F build still calls $SYM"
done

# The same float source under D: D implies F, so the float half must still use
# fadd.s while the ABI becomes ilp32d.
STATUS=0
run_fp float f32-id rv32id ilp32d +f,+d || STATUS=$?
[[ "$STATUS" -eq 0 ]] || fail "float program (rv32id/ilp32d) exited with $STATUS"
grep -q 'fadd.s' "$TMP/f32-id.s" || fail "the D build lost fadd.s"

# Doubles: the D-specific instructions must be there and the soft-float
# double libcalls must be gone.
STATUS=0
run_fp double f64-id rv32id ilp32d +f,+d || STATUS=$?
[[ "$STATUS" -eq 0 ]] || fail "double program (rv32id/ilp32d) exited with $STATUS"
grep -q 'fadd.d' "$TMP/f64-id.s" || fail "the D build emitted no fadd.d"
grep -q 'fcvt.w.d' "$TMP/f64-id.s" || fail "the D build emitted no fcvt.w.d"
grep -q 'fcvt.s.d' "$TMP/f64-id.s" || fail "the D build emitted no fcvt.s.d"
grep -q 'fcvt.d.s' "$TMP/f64-id.s" || fail "the D build emitted no fcvt.d.s"
grep -q 'fsd\|fld' "$TMP/f64-id.s" || fail "the D build used no FP spill/reload"
RELOCS=$("$READELF" -r "$TMP/f64-id.o")
for SYM in __adddf3 __muldf3 __eqdf2 __ltdf2 __fixdfsi __floatsidf \
           __extendsfdf2 __truncdfsf2; do
  [[ "$RELOCS" != *"$SYM"* ]] || fail "the D build still calls $SYM"
done

echo "PASS: F/D programs ran under $QEMU (rv32if + rv32id; 9-arg calls,"
echo "      f64 stack constants/select, and no soft-float libcalls)"
