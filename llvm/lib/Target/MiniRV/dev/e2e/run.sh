#!/usr/bin/env bash
# End-to-end differential test for MiniRV.
#
# Every .ll defines main and returns a small exit code.  The same IR is
# compiled for MiniRV and for the RISC-V reference backend, linked against a
# tiny start.s with ld.lld, run under qemu-riscv64-static, and the exit codes
# are compared.
#
#   LLVM_BUILD_DIR=<build> llvm/lib/Target/MiniRV/dev/e2e/run.sh
set -euo pipefail
TREE=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
BUILD=${LLVM_BUILD_DIR:-/home/ubuntu/llvm/builds/dsl-tablegen}
B="$BUILD/bin"
LDLLD=${LDLLD:-ld.lld}
QEMU=${QEMU:-qemu-riscv64-static}
cd "$(dirname "$0")"

for tool in "$B/llc" "$B/llvm-mc" "$LDLLD" "$QEMU"; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "skip: $tool not found" >&2
    exit 77
  fi
done

OUT=$(mktemp -d)
echo "artifacts: $OUT"

# start.s is plain RISC-V; the MiniRV assembler intentionally has no ecall or
# register aliases, so assemble it with the reference target.
"$B/llvm-mc" -triple=riscv64 -filetype=obj start.s -o "$OUT/start.o"

fail=0
for f in *.ll; do
  base=${f%.ll}
  "$B/llc" -mtriple=minirv64 -O2 -filetype=obj "$f" -o "$OUT/$base.m.o" \
    || { echo "FAIL $base: MiniRV compile"; fail=1; continue; }
  "$B/llc" -mtriple=riscv64 -mattr=+m -O2 -filetype=obj "$f" \
    -o "$OUT/$base.r.o" \
    || { echo "FAIL $base: riscv64 compile"; fail=1; continue; }
  "$LDLLD" -m elf64lriscv --no-relax "$OUT/start.o" "$OUT/$base.m.o" \
    -o "$OUT/$base.m.elf" \
    || { echo "FAIL $base: MiniRV link"; fail=1; continue; }
  "$LDLLD" -m elf64lriscv --no-relax "$OUT/start.o" "$OUT/$base.r.o" \
    -o "$OUT/$base.r.elf" \
    || { echo "FAIL $base: riscv64 link"; fail=1; continue; }

  "$QEMU" "$OUT/$base.m.elf" >/dev/null 2>&1 && m=0 || m=$?
  "$QEMU" "$OUT/$base.r.elf" >/dev/null 2>&1 && r=0 || r=$?
  if [ "$m" = "$r" ]; then
    printf 'OK   %-16s exit=%s\n' "$base" "$m"
  else
    printf 'FAIL %-16s minirv=%s riscv64=%s\n' "$base" "$m" "$r"
    fail=1
  fi
done

exit "$fail"
