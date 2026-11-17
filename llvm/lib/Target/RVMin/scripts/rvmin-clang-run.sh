#!/usr/bin/env bash
#
# Chapter 27 gate: build, link and run the runtime programs with a single
# clang driver, using `--target=rvmin-unknown-elf` end to end.  Unlike
# rvmin-run.sh (which deliberately splits clang/llc/llvm-mc to exercise each
# layer), this script only proves that the driver path works.
#
# Usage: rvmin-clang-run.sh <llvm-bin-dir> [qemu]
#
set -euo pipefail

BIN=${1:?usage: rvmin-clang-run.sh <llvm-bin-dir> [qemu]}
QEMU=${2:-qemu-riscv32-static}
CC="$BIN/clang"
READELF="$BIN/llvm-readelf"
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
RT="$HERE/rt"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

# build_and_run <program> <march> <opt> <output>
build_and_run() {
  local prog=$1 march=$2 opt=$3 out=$4
  local elf="$TMP/${prog}-${march}-${opt}"
  "$CC" --target=rvmin-unknown-elf -march="$march" -mabi=ilp32 -"$opt" \
    -ffreestanding -fno-builtin -nostdlib -fuse-ld=lld \
    -Wl,-T,"$RT/link.ld" "$RT/start.S" "$RT/rvmin-rt.S" "$RT/$prog.c" \
    -o "$elf" 2> "$elf.log" || { cat "$elf.log" >&2; fail "clang could not build $prog ($march, -$opt)"; }
  if [[ -s "$elf.log" ]]; then
    cat "$elf.log" >&2
    fail "clang printed diagnostics for $prog ($march, -$opt)"
  fi
  "$READELF" -h "$elf" | grep -q 'Machine:.*RISC-V' \
    || fail "$prog ($march) is not an EM_RISCV object"
  local status=0
  "$QEMU" "$elf" > "$out" || status=$?
  return "$status"
}

# 1. demo.c: the same C program in RV32I and RV32I+M must print the same text.
STATUS=0
build_and_run demo rv32i O2 "$TMP/demo-i.out" || STATUS=$?
[[ "$STATUS" -eq 0 ]] || fail "rv32i demo exited with $STATUS"
STATUS=0
build_and_run demo rv32im O2 "$TMP/demo-m.out" || STATUS=$?
[[ "$STATUS" -eq 0 ]] || fail "rv32im demo exited with $STATUS"
cmp -s "$TMP/demo-i.out" "$TMP/demo-m.out" \
  || fail "demo output differs between rv32i and rv32im"
echo "PASS: clang --target=rvmin linked and ran demo.c as rv32i and rv32im"

# 2. The rest of the runtime corpus, each through the same one-command path.
for O in O0 O2; do
  STATUS=0
  build_and_run large-frame rv32i "$O" "$TMP/lf-$O.out" || STATUS=$?
  [[ "$STATUS" -eq 0 ]] || fail "large-frame $O exited with $STATUS"
done
echo "PASS: 3 KiB frames ran under $QEMU at -O0 and -O2"

STATUS=0
build_and_run tail rv32i O2 "$TMP/tail.out" || STATUS=$?
[[ "$STATUS" -eq 0 ]] || fail "tail-call program exited with $STATUS"
echo "PASS: 1e6 tail calls ran under $QEMU"

STATUS=0
build_and_run switch rv32i O2 "$TMP/switch.out" || STATUS=$?
[[ "$STATUS" -eq 0 ]] || fail "switch program exited with $STATUS"
echo "PASS: jump-table switch program ran under $QEMU"

for march in rv32i rv32im; do
  STATUS=0
  build_and_run muldiv "$march" O2 "$TMP/muldiv-$march.out" || STATUS=$?
  [[ "$STATUS" -eq 0 ]] || fail "muldiv ($march) exited with $STATUS"
done
cmp -s "$TMP/muldiv-rv32i.out" "$TMP/muldiv-rv32im.out" \
  || fail "muldiv output differs between rv32i and rv32im"
echo "PASS: constant multiply/divide ran with and without M"

STATUS=0
build_and_run atomics rv32ia O2 "$TMP/atomics.out" || STATUS=$?
[[ "$STATUS" -eq 0 ]] || fail "atomics program exited with $STATUS"
echo "PASS: A-extension atomics ran under $QEMU"

# Chapter 51: the driver must accept `ilp32` together with hardware F/D.  The
# shared float/double corpora plus the dedicated ABI smoke program all run
# through the single clang command used by this script.
for spec in "float rv32if" "double rv32id" "abi-soft rv32id"; do
  set -- $spec
  STATUS=0
  build_and_run "$1" "$2" O2 "$TMP/$1-$2.out" || STATUS=$?
  [[ "$STATUS" -eq 0 ]] || fail "$1 ($2, ilp32) exited with $STATUS"
done
echo "PASS: ilp32 + hardware F/D ran through the clang driver"

echo "ALL CLANG-DRIVER RUNS PASS"
