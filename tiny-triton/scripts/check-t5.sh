#!/usr/bin/env bash
#
# Tiny Triton chapter 5 gate: TTGIR reaches the LLVM dialect and the simulator.
#
# Usage: check-t5.sh <llvm-bin-dir>
#
set -uo pipefail

BIN=${1:?usage: check-t5.sh <llvm-bin-dir>}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
SIM="$ROOT/../llvm/lib/Target/GPUMin/tools/gpumin_sim.py"
TMP=$(mktemp -d)
trap 'python3 -c "import shutil,sys; shutil.rmtree(sys.argv[1], ignore_errors=True)" "$TMP"' EXIT
export PYTHONDONTWRITEBYTECODE=1

OPT="$BIN/tiny-opt"
TRANSLATE="$BIN/mlir-translate"
LLC="$BIN/llc"
passed=0
failed=0

ok()  { passed=$((passed + 1)); echo "PASS $passed/5: $1"; }
bad() { failed=$((failed + 1)); echo "FAIL: $1"; }

# 1. The pass runs on the chapter 4 golden.
if "$OPT" --tiny-convert-ttg-to-llvm "$ROOT/tests/passes/vecadd.ttgir" \
      -o "$TMP/vecadd.llir" 2>"$TMP/convert.err"; then
  ok "--tiny-convert-ttg-to-llvm runs on vecadd.ttgir"
else
  bad "the pass failed: $(cat "$TMP/convert.err")"
fi

# 2. The result is LLVM dialect: no tensors, no tinyttg ops.
if grep -q 'llvm.func @vecadd' "$TMP/vecadd.llir" &&
   grep -q 'llvm.gpumin.args' "$TMP/vecadd.llir" &&
   ! grep -q 'tensor<' "$TMP/vecadd.llir" &&
   ! grep -qE 'tinyttg\.(get_program_id|make_range|splat|addptr|load|store|reduce|trans|broadcast|expand_dims|masked_|convert_layout|local_|barrier)' "$TMP/vecadd.llir"; then
  ok "every tensor is scalarised and the kernel uses the sr.args ABI"
else
  bad "the LLIR still contains tensor or tinyttg constructs"
fi

# 3. The result is the chapter 5 golden.
if diff -u "$ROOT/tests/passes/vecadd.llir" "$TMP/vecadd.llir" >"$TMP/diff.txt"; then
  ok "vecadd.llir matches the chapter 5 golden"
else
  bad "unexpected LLIR: $(head -20 "$TMP/diff.txt")"
fi

# 4. It translates to LLVM IR and compiles for GPUMin.
if "$TRANSLATE" --mlir-to-llvmir "$TMP/vecadd.llir" -o "$TMP/vecadd.ll" \
      2>"$TMP/translate.err" &&
   "$LLC" -march=gpumin -O1 -verify-machineinstrs "$TMP/vecadd.ll" \
      -o "$TMP/vecadd.s" 2>"$TMP/llc.err" &&
   grep -q 'sr.args' "$TMP/vecadd.s" &&
   grep -q 'sr.tid' "$TMP/vecadd.s" &&
   grep -q 'sr.blockidx' "$TMP/vecadd.s" &&
   grep -q $'^\texit$' "$TMP/vecadd.s"; then
  ok "mlir-translate and llc -march=gpumin accept the kernel"
else
  bad "translate/llc failed: $(cat "$TMP/translate.err" "$TMP/llc.err")"
fi

# 5. The kernel runs on the simulator and covers all 1024 elements.
cat > "$TMP/launch.json" <<'JSON'
{
  "grid": 1,
  "block": 32,
  "args_addr": 4096,
  "args": [0, 8192, 16384],
  "fill": [
    { "addr": 0, "start": 1065353216, "step": 0, "count": 512 },
    { "addr": 2048, "start": 1073741824, "step": 0, "count": 512 },
    { "addr": 8192, "start": 0, "step": 0, "count": 1024 }
  ],
  "dump": [ { "addr": 16384, "count": 1024 } ]
}
JSON
if "$LLC" -march=gpumin -O1 -filetype=obj "$TMP/vecadd.ll" -o "$TMP/vecadd.o" \
      2>"$TMP/obj.err" &&
   python3 "$SIM" "$TMP/vecadd.o" --kernel vecadd --launch "$TMP/launch.json" \
      2>"$TMP/sim.err" >"$TMP/dump.txt" &&
   python3 - "$TMP/dump.txt" <<'PY'
import sys
words = [int(w) for w in open(sys.argv[1]).read().split()]
one, two = 1065353216, 1073741824
ok = len(words) == 1024 and all(w == one for w in words[:512]) and all(
    w == two for w in words[512:])
sys.exit(0 if ok else 1)
PY
then
  ok "all 1024 elements are written by the right thread"
else
  bad "simulator mismatch: $(cat "$TMP/sim.err" 2>/dev/null; head -c 200 "$TMP/dump.txt" 2>/dev/null)"
fi

echo "----"
echo "chapter 5: $passed passed, $failed failed"
[ "$failed" -eq 0 ]
