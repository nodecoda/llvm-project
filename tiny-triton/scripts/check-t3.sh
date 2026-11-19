#!/usr/bin/env bash
#
# Tiny Triton chapter 3 gate: `tinyttg` is TTIR plus a thread mapping.
#
# Usage: check-t3.sh <llvm-bin-dir>
#
set -uo pipefail

BIN=${1:?usage: check-t3.sh <llvm-bin-dir>}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
TMP=$(mktemp -d)
trap 'python3 -c "import shutil,sys; shutil.rmtree(sys.argv[1], ignore_errors=True)" "$TMP"' EXIT
export PYTHONDONTWRITEBYTECODE=1

OPT="$BIN/tiny-opt"
passed=0
failed=0

ok()  { passed=$((passed + 1)); echo "PASS $passed/5: $1"; }
bad() { failed=$((failed + 1)); echo "FAIL: $1"; }

# 1. The dialect is advertised by the driver.
if "$OPT" --help >"$TMP/help.txt" 2>&1 && grep -q 'tinyttg' "$TMP/help.txt"; then
  ok "tinyttg appears in --help"
else
  bad "tinyttg is not advertised by tiny-opt --help"
fi

# 2. The hand-written TTGIR golden parses, verifies and round-trips.
if "$OPT" --verify-each "$ROOT/tests/ttg/vecadd.ttgir" -o "$TMP/a.mlir" \
      2>"$TMP/parse.err"; then
  if "$OPT" "$TMP/a.mlir" -o "$TMP/b.mlir" 2>>"$TMP/parse.err" &&
     diff -q "$TMP/a.mlir" "$TMP/b.mlir" >/dev/null; then
    ok "vecadd.ttgir round-trips through tiny-opt"
  else
    bad "vecadd.ttgir does not round-trip: $(cat "$TMP/parse.err")"
  fi
else
  bad "vecadd.ttgir rejected: $(cat "$TMP/parse.err")"
fi

# 3. A tensor without a layout is rejected (layouts are not decoration).
if "$OPT" "$ROOT/tests/negative/ttg-missing-layout.ttir" >/dev/null \
      2>"$TMP/missing.err"; then
  bad "a tinyttg tensor without a layout was accepted"
elif grep -q 'must carry a #tinyttg layout' "$TMP/missing.err"; then
  ok "missing layouts are rejected"
else
  bad "wrong diagnostic for a missing layout: $(cat "$TMP/missing.err")"
fi

# 4. A blocked layout that does not tile the shape is rejected.
if "$OPT" "$ROOT/tests/negative/ttg-bad-tile.ttir" >/dev/null \
      2>"$TMP/tile.err"; then
  bad "a tensor that the layout cannot tile was accepted"
elif grep -q 'not divisible by layout tile' "$TMP/tile.err"; then
  ok "non-tiling layouts are rejected"
else
  bad "wrong diagnostic for a bad tile: $(cat "$TMP/tile.err")"
fi

# 5. Chapters 1 and 2 still pass unchanged.
if "$HERE/check-t1.sh" "$BIN" >"$TMP/t1.txt" 2>&1 &&
   "$HERE/check-t2.sh" "$BIN" >"$TMP/t2.txt" 2>&1; then
  ok "check-t1.sh and check-t2.sh still pass"
else
  bad "earlier chapters regressed: $(tail -n 5 "$TMP/t1.txt" "$TMP/t2.txt")"
fi

echo "----"
echo "chapter 3: $passed passed, $failed failed"
[ "$failed" -eq 0 ]
