#!/usr/bin/env bash
#
# Tiny Triton chapter 4 gate: TTIR gains a thread mapping.
#
# Usage: check-t4.sh <llvm-bin-dir>
#
set -uo pipefail

BIN=${1:?usage: check-t4.sh <llvm-bin-dir>}
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

# 1. The pass runs on the chapter 2 golden.
if "$OPT" --tiny-convert-tt-to-ttg "$ROOT/kernels/vecadd.ttir" -o "$TMP/out.ttgir" \
      2>"$TMP/convert.err"; then
  ok "--tiny-convert-tt-to-ttg runs on kernels/vecadd.ttir"
else
  bad "the pass failed: $(cat "$TMP/convert.err")"
fi

# 2. The result is the chapter 4 golden, byte for byte.
if diff -u "$ROOT/tests/passes/vecadd.ttgir" "$TMP/out.ttgir" >"$TMP/diff.txt"; then
  ok "vecadd.ttgir matches the chapter 4 golden"
else
  bad "unexpected TTGIR: $(head -20 "$TMP/diff.txt")"
fi

# 3. Every tensor carries a layout, and the ops moved dialect.
if "$OPT" --verify-each "$TMP/out.ttgir" -o /dev/null 2>"$TMP/verify.err" &&
   grep -q 'tinyttg.addptr' "$TMP/out.ttgir" &&
   ! grep -qE '(^|[[:space:]])tinytt\.' "$TMP/out.ttgir"; then
  ok "the result is verifiable tinyttg with no tinytt ops left"
else
  bad "the result is not clean tinyttg: $(cat "$TMP/verify.err")"
fi

# 4. The pass is a real pass: advertised and runnable through a pipeline.
if "$OPT" --help >"$TMP/help.txt" 2>&1 &&
   grep -q 'tiny-convert-tt-to-ttg' "$TMP/help.txt" &&
   "$OPT" --pass-pipeline='builtin.module(tiny-convert-tt-to-ttg)' \
     "$ROOT/kernels/vecadd.ttir" -o "$TMP/pipeline.ttgir" 2>"$TMP/pipe.err" &&
   diff -q "$TMP/pipeline.ttgir" "$TMP/out.ttgir" >/dev/null; then
  ok "the pass is advertised and runs through --pass-pipeline"
else
  bad "pipeline invocation failed: $(cat "$TMP/pipe.err" 2>/dev/null)"
fi

# 5. A tensor the layout cannot tile is rejected, not silently masked.
if "$OPT" --tiny-convert-tt-to-ttg "$ROOT/tests/negative/ttg-convert-bad-size.ttir" \
      >/dev/null 2>"$TMP/bad.err"; then
  bad "a 100-element tensor was assigned a 32-thread layout"
elif grep -q 'not divisible' "$TMP/bad.err"; then
  ok "non-tiling tensors are rejected with a location"
else
  bad "wrong diagnostic: $(cat "$TMP/bad.err")"
fi

echo "----"
echo "chapter 4: $passed passed, $failed failed"
[ "$failed" -eq 0 ]
