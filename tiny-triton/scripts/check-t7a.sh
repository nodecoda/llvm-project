#!/usr/bin/env bash
#
# Tiny Triton chapter 7a gate: a 2-D index tile built from `arange` column and
# row vectors copies 64 floats through a 2-D blocked layout.
#
# Usage: check-t7a.sh <llvm-bin-dir>
#
set -uo pipefail

BIN=${1:?usage: check-t7a.sh <llvm-bin-dir>}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
SIM="$ROOT/../llvm/lib/Target/GPUMin/tools/gpumin_sim.py"
TMP=$(mktemp -d)
trap 'python3 -c "import shutil,sys; shutil.rmtree(sys.argv[1], ignore_errors=True)" "$TMP"' EXIT
export PYTHONDONTWRITEBYTECODE=1

FRONTEND="$BIN/tiny-triton"
OPT="$BIN/tiny-opt"
TRANSLATE="$BIN/mlir-translate"
LLC="$BIN/llc"
passed=0
failed=0

ok()  { passed=$((passed + 1)); echo "PASS $passed/5: $1"; }
bad() { failed=$((failed + 1)); echo "FAIL: $1"; }

T=copy2d
SRC="$ROOT/kernels/$T.py"
ARGS=(--arg-type in_ptr=ptr --arg-type out_ptr=ptr)

# 1. The frontend turns `[:, None]` / `[None, :]` into expand_dims and grows
#    the two vectors to one 8x8 tile with broadcast.
if "$FRONTEND" "$SRC" --emit ttir "${ARGS[@]}" -o "$TMP/$T.ttir" \
      2>"$TMP/frontend.err" &&
   "$OPT" --verify-each "$TMP/$T.ttir" -o /dev/null 2>>"$TMP/frontend.err" &&
   grep -q 'tinytt.expand_dims .*tensor<8xi32> -> tensor<8x1xi32>' "$TMP/$T.ttir" &&
   grep -q 'tinytt.expand_dims .*tensor<8xi32> -> tensor<1x8xi32>' "$TMP/$T.ttir" &&
   grep -q 'tinytt.broadcast .*tensor<8x1xi32> -> tensor<8x8xi32>' "$TMP/$T.ttir" &&
   grep -q 'tinytt.broadcast .*tensor<1x8xi32> -> tensor<8x8xi32>' "$TMP/$T.ttir" &&
   grep -q 'tinytt.addptr .*tensor<8x8xi32>' "$TMP/$T.ttir" &&
   grep -q 'tinytt.store' "$TMP/$T.ttir"; then
  ok "the frontend emits expand_dims + broadcast for an 8x8 index tile"
else
  bad "frontend did not produce the chapter 7a TTIR: $(cat "$TMP/frontend.err")"
fi

# 2. T4 gives every 2-D tensor a blocked layout, and only 1-D small ranges get
#    a single-warp layout.
if "$OPT" --tiny-convert-tt-to-ttg "$TMP/$T.ttir" -o "$TMP/$T.ttgir" \
      2>"$TMP/t4.err" &&
   diff -u "$ROOT/tests/passes/$T.ttgir" "$TMP/$T.ttgir" >"$TMP/t4.diff" &&
   grep -q 'threadsPerWarp = \[1, 8\], warpsPerCTA = \[4, 1\]' "$TMP/$T.ttgir" &&
   grep -q 'threadsPerWarp = \[8\], warpsPerCTA = \[1\]' "$TMP/$T.ttgir" &&
   ! grep -qE 'tensor<8x8[a-z0-9]*>' "$TMP/$T.ttgir"; then
  ok "TTGIR assigns a 4-warp 2-D layout and a replicated 1-D range layout"
else
  bad "layout assignment failed: $(cat "$TMP/t4.err" 2>/dev/null; head -20 "$TMP/t4.diff" 2>/dev/null)"
fi

# 3. T5 computes row and column coordinates from tid and keeps the offsets.
if "$OPT" --tiny-convert-ttg-to-llvm "$TMP/$T.ttgir" -o "$TMP/$T.llir" \
      2>"$TMP/t5.err" &&
   diff -u "$ROOT/tests/passes/$T.llir" "$TMP/$T.llir" >"$TMP/t5.diff" &&
   ! grep -q 'tensor<' "$TMP/$T.llir" &&
   ! grep -qE 'tinyttg\.(make_range|expand_dims|broadcast|splat|addptr|load|store)' "$TMP/$T.llir"; then
  ok "LLIR scalarises the 2-D tile and matches its golden"
else
  bad "TTG->LLVM failed: $(cat "$TMP/t5.err" 2>/dev/null; head -20 "$TMP/t5.diff" 2>/dev/null)"
fi

# 4. The result compiles for GPUMin.
if "$TRANSLATE" --mlir-to-llvmir "$TMP/$T.llir" -o "$TMP/$T.ll" \
      2>"$TMP/translate.err" &&
   "$LLC" -march=gpumin -O1 -verify-machineinstrs "$TMP/$T.ll" -o "$TMP/$T.s" \
      2>"$TMP/llc.err" &&
   grep -q 'sr.tid' "$TMP/$T.s" &&
   grep -q $'^\texit$' "$TMP/$T.s"; then
  ok "mlir-translate and llc -march=gpumin accept the 2-D kernel"
else
  bad "translate/llc failed: $(cat "$TMP/translate.err" "$TMP/llc.err" 2>/dev/null)"
fi

# 5. The simulator copies all 64 elements to the same linear index.  The
#    source is 0..63, so a row/column mix-up shows up as a permuted dump.
cat > "$TMP/launch.json" <<'JSON'
{
  "grid": 1,
  "block": 32,
  "args_addr": 4096,
  "args": [0, 8192],
  "fill": [
    { "addr": 0, "start": 0, "step": 1, "count": 64 },
    { "addr": 8192, "start": 0, "step": 0, "count": 64 }
  ],
  "dump": [ { "addr": 8192, "count": 64 } ]
}
JSON
if "$LLC" -march=gpumin -O1 -filetype=obj "$TMP/$T.ll" -o "$TMP/$T.o" \
      2>"$TMP/obj.err" &&
   python3 "$SIM" "$TMP/$T.o" --kernel $T --launch "$TMP/launch.json" \
      2>"$TMP/sim.err" >"$TMP/dump.txt" &&
   python3 - "$TMP/dump.txt" <<'PY'
import sys
words = [int(w) for w in open(sys.argv[1]).read().split()]
sys.exit(0 if words == list(range(64)) else 1)
PY
then
  ok "the simulator copies 64 elements to the right 2-D offsets"
else
  bad "simulator mismatch: $(cat "$TMP/sim.err" 2>/dev/null; head -c 200 "$TMP/dump.txt" 2>/dev/null)"
fi

echo "----"
echo "chapter 7a: $passed passed, $failed failed"
[ "$failed" -eq 0 ]
