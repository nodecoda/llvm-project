#!/usr/bin/env bash
#
# Chapter 00 gate: the GPUMin target exists and can produce a listing.
#
# Usage: check-ch00.sh <llvm-bin-dir>
#
# Four checks, in increasing order of what they prove:
#   1. the target is registered under its own Triple arch;
#   2. an empty module produces a listing (the codegen pipeline runs);
#   3. `ret void` in a kernel becomes the GM32 `exit` word (ISel + pseudo
#      expansion; chapter 07 adds the device-function `ret`);
#   4. `exit` encodes to 0x48000000 (opcode 0x12 in bits 31:26).
#
set -euo pipefail

BIN=${1:?usage: check-ch00.sh <llvm-bin-dir>}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

"$BIN/llc" --version | grep -qw gpumin || fail "llc --version does not list gpumin"
echo "PASS 1/4: target registered"

printf 'target triple = "gpumin-unknown-elf"\n' > "$TMP/empty.ll"
"$BIN/llc" -march=gpumin "$TMP/empty.ll" -o "$TMP/empty.s"
grep -q '\.file' "$TMP/empty.s" || fail "empty module produced no listing"
echo "PASS 2/4: empty module emits a listing"

cat > "$TMP/ret.ll" <<'IR'
target triple = "gpumin-unknown-elf"
define void @k() #0 {
  ret void
}
attributes #0 = { "gpumin_kernel" }
IR
"$BIN/llc" -march=gpumin "$TMP/ret.ll" -o "$TMP/ret.s"
grep -qw 'exit' "$TMP/ret.s" || fail "ret void did not lower to exit"
echo "PASS 3/4: ret void lowers to exit"

ENC=$(printf 'exit\n' | "$BIN/llvm-mc" -triple=gpumin -show-encoding | grep -o '\[0x00,0x00,0x00,0x48\]')
[ -n "$ENC" ] || fail "exit does not encode as 0x48000000"
echo "PASS 4/4: exit encodes as 0x48000000"

echo "OK: chapter 00 gate passed"
