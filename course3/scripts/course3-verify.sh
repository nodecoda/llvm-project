#!/usr/bin/env bash
#
# Course 3 gate.  Every chapter adds checks here; chapter 00 has none yet,
# which is why the script prints "0 checks".
#
# Usage: course3-verify.sh <llvm-bin-dir> <rmach-binary>
#
set -euo pipefail

BIN=${1:?usage: course3-verify.sh <llvm-bin-dir> <rmach-binary>}
RMACH=${2:?usage: course3-verify.sh <llvm-bin-dir> <rmach-binary>}

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

[[ -x "$RMACH" ]] || fail "$RMACH is not executable"

# A smoke test, deliberately not counted as a chapter check.
"$RMACH" --version | grep -q '^rmach ' || fail "rmach --version is not rmach"

CHECKS=0

echo "$CHECKS checks"
