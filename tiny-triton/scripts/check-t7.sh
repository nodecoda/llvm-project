#!/usr/bin/env bash
#
# Tiny Triton chapter 7 gate: run both halves in order.  7a covers 2-D
# layouts, 7b covers the parts that must cross threads through shared memory.
#
# Usage: check-t7.sh <llvm-bin-dir>
#
set -uo pipefail

BIN=${1:?usage: check-t7.sh <llvm-bin-dir>}
HERE=$(cd "$(dirname "$0")" && pwd)

"$HERE/check-t7a.sh" "$BIN" || exit 1
"$HERE/check-t7b.sh" "$BIN" || exit 1
