#!/usr/bin/env bash
#
# Tiny Triton chapter 1 gate: `tinytt` is a real dialect driven by `tiny-opt`.
#
# Usage: check-t1.sh <llvm-bin-dir>
#
set -uo pipefail

BIN=${1:?usage: check-t1.sh <llvm-bin-dir>}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
TMP=$(mktemp -d)
trap 'python3 -c "import shutil,sys; shutil.rmtree(sys.argv[1], ignore_errors=True)" "$TMP"' EXIT

OPT="$BIN/tiny-opt"
passed=0
failed=0

ok()  { passed=$((passed + 1)); echo "PASS $passed/5: $1"; }
bad() { failed=$((failed + 1)); echo "FAIL: $1"; }

# 1. The driver builds and runs.
if "$OPT" --version >/dev/null 2>&1; then
  ok "tiny-opt runs"
else
  bad "tiny-opt is missing or broken at $OPT"
fi

# 2. The smallest legal module round-trips.
if "$OPT" --verify-each "$ROOT/tests/empty.mlir" >"$TMP/empty.out" 2>"$TMP/empty.err"; then
  ok "empty module roundtrip"
else
  bad "empty module rejected: $(cat "$TMP/empty.err")"
fi

# 3. The vecadd TTIR golden parses, verifies and reprints.
if "$OPT" --verify-each "$ROOT/kernels/vecadd.ttir" >"$TMP/vecadd.out" 2>"$TMP/vecadd.err"; then
  grep -q 'tinytt.addptr' "$TMP/vecadd.out" \
    && ok "vecadd.ttir parses and reprints" \
    || bad "vecadd.ttir reprint lost tinytt.addptr"
else
  bad "vecadd.ttir rejected: $(cat "$TMP/vecadd.err")"
fi

# 4. The verifiers reject illegal TTIR (this is what makes tinytt a dialect).
negative_ok=1
for name in load-non-pointer reduce-bad-combine; do
  if "$OPT" "$ROOT/tests/negative/$name.ttir" >/dev/null 2>"$TMP/$name.err"; then
    bad "negative case '$name' was accepted"
    negative_ok=0
  fi
done
[ "$negative_ok" = 1 ] && ok "verifier rejects illegal TTIR"

# 5. The dialect is advertised by the driver.
if "$OPT" --help >"$TMP/help.txt" 2>&1 && grep -q 'tinytt' "$TMP/help.txt"; then
  ok "tinytt appears in --help"
else
  bad "tinytt is not advertised by tiny-opt --help"
fi

echo "----"
echo "chapter 1: $passed passed, $failed failed"
[ "$failed" -eq 0 ]
