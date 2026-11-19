#!/usr/bin/env bash
#
# Run every tiny-triton gate, then the two GPUMin gates this course depends on.
# This is the single command the course uses to declare the tree green.
#
# Usage: verify-all.sh <llvm-bin-dir>
#
set -uo pipefail

BIN=${1:?usage: verify-all.sh <llvm-bin-dir>}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)

for n in 1 2 3 4 5 6 7 8; do
  echo "== check-t$n.sh =="
  "$HERE/check-t$n.sh" "$BIN" || { echo "verify-all: check-t$n.sh failed"; exit 1; }
done

echo "== gpumin-verify-all.sh =="
"$ROOT/../llvm/lib/Target/GPUMin/scripts/gpumin-verify-all.sh" "$BIN" || {
  echo "verify-all: gpumin-verify-all.sh failed"
  exit 1
}

echo "== triton-verify-all.sh =="
"$ROOT/../llvm/lib/Target/GPUMin/scripts/triton-verify-all.sh" "$BIN" || {
  echo "verify-all: triton-verify-all.sh failed"
  exit 1
}

echo "verify-all: all tiny-triton chapters and GPUMin gates are green"
