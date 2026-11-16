#!/usr/bin/env bash
#
# Course 2 gate.  Every chapter adds checks here; chapter 00 has none yet,
# which is why the script prints "0 checks".
#
# Usage: course2-verify.sh <rsim-binary>
#
set -euo pipefail

BIN=${1:?usage: course2-verify.sh <rsim-binary>}

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

[[ -x "$BIN" ]] || fail "$BIN is not executable"

# A smoke test, deliberately not counted as a chapter check.
"$BIN" --version | grep -q '^rsim ' || fail "rsim --version is not rsim"

CHECKS=0

echo "$CHECKS checks"
