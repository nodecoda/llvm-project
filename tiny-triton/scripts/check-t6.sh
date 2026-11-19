#!/usr/bin/env bash
#
# Tiny Triton chapter 6 gate: a 1-D reduction crosses shared memory and a
# barrier, then writes one scalar.
#
# Usage: check-t6.sh <llvm-bin-dir>
#
set -uo pipefail

BIN=${1:?usage: check-t6.sh <llvm-bin-dir>}
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

# 1. The frontend emits a scalar reduce and a scalar store.
if "$FRONTEND" "$ROOT/kernels/reduce_sum.py" --emit ttir \
      --arg-type x_ptr=ptr --arg-type out_ptr=ptr \
      -o "$TMP/reduce_sum.ttir" 2>"$TMP/frontend.err" &&
   "$OPT" --verify-each "$TMP/reduce_sum.ttir" -o /dev/null \
      2>>"$TMP/frontend.err" &&
   grep -q -- 'tensor<1024xf32> -> f32' "$TMP/reduce_sum.ttir" &&
   grep -q 'tinytt.store_scalar' "$TMP/reduce_sum.ttir"; then
  ok "tl.sum lowers to a scalar reduce and store_scalar"
else
  bad "frontend did not produce the chapter 6 TTIR: $(cat "$TMP/frontend.err")"
fi

# 2. The two conversions match the goldens and leave no tensor behind.
if "$OPT" --tiny-convert-tt-to-ttg "$TMP/reduce_sum.ttir" \
      -o "$TMP/reduce_sum.ttgir" 2>"$TMP/t4.err" &&
   diff -u "$ROOT/tests/passes/reduce_sum.ttgir" "$TMP/reduce_sum.ttgir" \
      >"$TMP/t4.diff" &&
   "$OPT" --tiny-convert-ttg-to-llvm "$TMP/reduce_sum.ttgir" \
      -o "$TMP/reduce_sum.llir" 2>"$TMP/t5.err" &&
   diff -u "$ROOT/tests/passes/reduce_sum.llir" "$TMP/reduce_sum.llir" \
      >"$TMP/t5.diff" &&
   grep -q 'llvm.gpumin.barrier' "$TMP/reduce_sum.llir" &&
   grep -q '!llvm.ptr<2>' "$TMP/reduce_sum.llir" &&
   grep -q 'llvm.cond_br' "$TMP/reduce_sum.llir" &&
   ! grep -q 'tensor<' "$TMP/reduce_sum.llir" &&
   ! grep -qE 'tinyttg\.(make_range|splat|addptr|load|store|store_scalar|reduce)' "$TMP/reduce_sum.llir"; then
  ok "TTGIR and LLIR match their goldens and use shared + barrier"
else
  bad "conversion mismatch: $(cat "$TMP/t4.err" "$TMP/t5.err" 2>/dev/null; head -20 "$TMP/t4.diff" "$TMP/t5.diff" 2>/dev/null)"
fi

# 3. The LLVM dialect compiles and the assembly really contains shared access.
if "$TRANSLATE" --mlir-to-llvmir "$TMP/reduce_sum.llir" -o "$TMP/reduce_sum.ll" \
      2>"$TMP/translate.err" &&
   "$LLC" -march=gpumin -O1 -verify-machineinstrs "$TMP/reduce_sum.ll" \
      -o "$TMP/reduce_sum.s" 2>"$TMP/llc.err" &&
   grep -q 'bar.sync' "$TMP/reduce_sum.s" &&
   grep -q 'sr.shared' "$TMP/reduce_sum.s" &&
   grep -q 'st.shared' "$TMP/reduce_sum.s" &&
   grep -q 'ld.shared' "$TMP/reduce_sum.s"; then
  ok "mlir-translate/llc emit bar.sync and shared loads/stores"
else
  bad "translate/llc failed: $(cat "$TMP/translate.err" "$TMP/llc.err" 2>/dev/null)"
fi

# 4. Both 1024 and 512 elements produce the right block sum.  The 512 case
#    catches a lowering that reads one shared slot per element per thread
#    instead of one slot per thread.
run_reduce() {
  local name=$1 py=$2 count=$3 want=$4
  "$FRONTEND" "$py" --emit ttir --arg-type x_ptr=ptr --arg-type out_ptr=ptr \
      -o "$TMP/$name.ttir" 2>"$TMP/$name.front.err" || return 1
  "$OPT" --tiny-convert-tt-to-ttg "$TMP/$name.ttir" \
      -o "$TMP/$name.ttgir" 2>"$TMP/$name.t4.err" || return 1
  "$OPT" --tiny-convert-ttg-to-llvm "$TMP/$name.ttgir" \
      -o "$TMP/$name.llir" 2>"$TMP/$name.t5.err" || return 1
  "$TRANSLATE" --mlir-to-llvmir "$TMP/$name.llir" -o "$TMP/$name.ll" \
      2>"$TMP/$name.tr.err" || return 1
  "$LLC" -march=gpumin -O1 -filetype=obj "$TMP/$name.ll" \
      -o "$TMP/$name.o" 2>"$TMP/$name.obj.err" || return 1
  cat > "$TMP/$name.launch.json" <<JSON
{
  "grid": 1,
  "block": 32,
  "args_addr": 4096,
  "args": [0, 8192],
  "fill": [ { "addr": 0, "start": 1065353216, "step": 0, "count": $count } ],
  "dump": [ { "addr": 8192, "count": 1 } ]
}
JSON
  python3 "$SIM" "$TMP/$name.o" --kernel "$name" --launch "$TMP/$name.launch.json" \
      >"$TMP/$name.dump" 2>"$TMP/$name.sim.err" || return 1
  python3 - "$TMP/$name.dump" "$want" <<'PY'
import struct, sys
word = int(open(sys.argv[1]).read().split()[0])
want = int(sys.argv[2], 0)
value = struct.unpack("<f", struct.pack("<I", word))[0]
print(f"  {sys.argv[1]}: {value}")
sys.exit(0 if word == want else 1)
PY
}

if run_reduce reduce_sum "$ROOT/kernels/reduce_sum.py" 1024 0x44800000 &&
   run_reduce reduce512 "$ROOT/tests/frontend/reduce512.py" 512 0x44000000; then
  ok "1024 and 512 element reductions both produce the right sum"
else
  bad "simulator mismatch: $(cat "$TMP"/reduce_sum.sim.err "$TMP"/reduce512.sim.err 2>/dev/null)"
fi

# 5. The verifier rejects illegal reductions and scalar stores.
negative_ok=1
check_negative() {
  local file=$1 pattern=$2
  if "$OPT" "$ROOT/tests/negative/$file" >/dev/null 2>"$TMP/$file.err"; then
    bad "negative case '$file' was accepted"
    negative_ok=0
  elif ! grep -q "$pattern" "$TMP/$file.err"; then
    bad "wrong diagnostic for '$file': $(cat "$TMP/$file.err")"
    negative_ok=0
  fi
}
check_negative reduce-tensor-result.ttir 'reduce produces a scalar'
check_negative reduce-bad-combine.ttir 'combine must be'
check_negative store-scalar-tensor.ttir 'value must be a scalar'
[ "$negative_ok" = 1 ] && ok "illegal reduce/store_scalar forms are rejected"

echo "----"
echo "chapter 6: $passed passed, $failed failed"
[ "$failed" -eq 0 ]
