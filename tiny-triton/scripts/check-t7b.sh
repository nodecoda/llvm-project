#!/usr/bin/env bash
#
# Tiny Triton chapter 7b gate: transpose through shared memory, a real layout
# conversion, a base-2 softmax and a 32^3 matmul on a 2-D grid.
#
# Usage: check-t7b.sh <llvm-bin-dir>
#
set -uo pipefail

BIN=${1:?usage: check-t7b.sh <llvm-bin-dir>}
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

# build <name> <py> <extra frontend args...>: kernel.py -> ttir -> ttgir -> llir -> .ll -> .o
build() {
  local name=$1 py=$2
  shift 2
  "$FRONTEND" "$py" --emit ttir "$@" -o "$TMP/$name.ttir" \
      2>"$TMP/$name.front.err" || return 1
  "$OPT" --tiny-convert-tt-to-ttg "$TMP/$name.ttir" -o "$TMP/$name.ttgir" \
      2>"$TMP/$name.t4.err" || return 1
  "$OPT" --tiny-convert-ttg-to-llvm "$TMP/$name.ttgir" -o "$TMP/$name.llir" \
      2>"$TMP/$name.t5.err" || return 1
  "$TRANSLATE" --mlir-to-llvmir "$TMP/$name.llir" -o "$TMP/$name.ll" \
      2>"$TMP/$name.tr.err" || return 1
  "$LLC" -march=gpumin -O1 -filetype=obj "$TMP/$name.ll" -o "$TMP/$name.o" \
      2>"$TMP/$name.obj.err"
}

# 1. transpose2d: trans crosses shared memory, and the simulator sees a real
#    permutation rather than a copy.
if build transpose2d "$ROOT/kernels/transpose2d.py" \
      --arg-type in_ptr=ptr --arg-type out_ptr=ptr &&
   grep -q 'tinyttg.trans .*<order = \[1, 0\]>' "$TMP/transpose2d.ttgir" &&
   grep -q 'llvm.gpumin.barrier' "$TMP/transpose2d.llir" &&
   grep -q '!llvm.ptr<2>' "$TMP/transpose2d.llir" &&
   diff -u "$ROOT/tests/passes/transpose2d.llir" "$TMP/transpose2d.llir" \
      >"$TMP/trans.diff"; then
  cat > "$TMP/transpose2d.launch" <<'JSON'
{ "grid": 1, "block": 32, "args_addr": 16384, "args": [0, 8192],
  "fill": [ { "addr": 0, "start": 0, "step": 1, "count": 64 },
            { "addr": 8192, "start": 0, "step": 0, "count": 64 } ],
  "dump": [ { "addr": 8192, "count": 64 } ] }
JSON
  if python3 "$SIM" "$TMP/transpose2d.o" --kernel transpose2d \
        --launch "$TMP/transpose2d.launch" 2>"$TMP/trans.sim.err" \
        >"$TMP/trans.dump" &&
     python3 - "$TMP/trans.dump" <<'PY'
import sys
words = [int(w) for w in open(sys.argv[1]).read().split()]
want = [(i % 8) * 8 + (i // 8) for i in range(64)]
sys.exit(0 if words == want else 1)
PY
  then
    ok "tl.trans goes through shared memory and really transposes 8x8"
  else
    bad "transpose mismatch: $(cat "$TMP/trans.sim.err" 2>/dev/null; head -c 200 "$TMP/trans.dump" 2>/dev/null)"
  fi
else
  bad "transpose lowering failed: $(cat "$TMP"/transpose2d.t*.err 2>/dev/null; head -20 "$TMP/trans.diff" 2>/dev/null)"
fi

# 2. A hand-written convert_layout: same shape, different distribution, so the
#    registers have to move.  The golden pins the shared round trip.
if "$OPT" --verify-each "$ROOT/tests/ttg/convert_layout.ttgir" -o /dev/null \
      2>"$TMP/cl.err" &&
   "$OPT" --tiny-convert-ttg-to-llvm "$ROOT/tests/ttg/convert_layout.ttgir" \
      -o "$TMP/convert_layout.llir" 2>>"$TMP/cl.err" &&
   diff -u "$ROOT/tests/passes/convert_layout.llir" "$TMP/convert_layout.llir" \
      >"$TMP/cl.diff" &&
   [ "$(grep -c 'llvm.store .*!llvm.ptr<2>' "$TMP/convert_layout.llir")" -ge 2 ] &&
   [ "$(grep -c 'llvm.load .*!llvm.ptr<2>' "$TMP/convert_layout.llir")" -ge 8 ] &&
   grep -q 'llvm.gpumin.barrier' "$TMP/convert_layout.llir"; then
  build_ok=1
  if "$TRANSLATE" --mlir-to-llvmir "$TMP/convert_layout.llir" -o "$TMP/cl.ll" \
       2>"$TMP/cl.tr.err" &&
     "$LLC" -march=gpumin -O1 -filetype=obj "$TMP/cl.ll" -o "$TMP/cl.o" \
       2>"$TMP/cl.obj.err"; then
    cat > "$TMP/cl.launch" <<'JSON'
{ "grid": 1, "block": 32, "args_addr": 4096, "args": [8192],
  "fill": [ { "addr": 8192, "start": 0, "step": 0, "count": 8 } ],
  "dump": [ { "addr": 8192, "count": 1 } ] }
JSON
    if python3 "$SIM" "$TMP/cl.o" --kernel convert_layout \
          --launch "$TMP/cl.launch" 2>"$TMP/cl.sim.err" >"$TMP/cl.dump" &&
       [ "$(cat "$TMP/cl.dump")" = "1065353216" ]; then
      ok "convert_layout moves an 8x8 tile between two layouts"
    else
      build_ok=0
    fi
  else
    build_ok=0
  fi
  [ "$build_ok" = 1 ] || bad "convert_layout did not run: $(cat "$TMP"/cl.*.err 2>/dev/null | head -5)"
else
  bad "convert_layout lowering failed: $(cat "$TMP/cl.err" 2>/dev/null; head -20 "$TMP/cl.diff" 2>/dev/null)"
fi

# 3. softmax2d: two row reductions, exp2 and a broadcast divide.
if build softmax2d "$ROOT/kernels/softmax2d.py" \
      --arg-type in_ptr=ptr --arg-type out_ptr=ptr &&
   [ "$(grep -c 'tinyttg.reduce' "$TMP/softmax2d.ttgir")" -eq 2 ] &&
   grep -q 'tinyttg.exp2' "$TMP/softmax2d.ttgir"; then
  if grep -q 'exp2' "$TMP/softmax2d.ll" &&
     "$LLC" -march=gpumin -O1 -verify-machineinstrs "$TMP/softmax2d.ll" \
        -o "$TMP/softmax2d.s" 2>"$TMP/softmax2d.llc.err" &&
     grep -q 'exp2' "$TMP/softmax2d.s" &&
     grep -q 'bar.sync' "$TMP/softmax2d.s"; then
    cat > "$TMP/softmax2d.launch" <<'JSON'
{ "grid": 1, "block": 32, "args_addr": 16384, "args": [0, 8192],
  "fill": [ { "addr": 0, "start": -4.0, "step": 0.125, "count": 64, "format": "f32" },
            { "addr": 8192, "start": 0.0, "step": 0.0, "count": 64, "format": "f32" } ],
  "dump": [ { "addr": 8192, "count": 64 } ] }
JSON
    if python3 "$SIM" "$TMP/softmax2d.o" --kernel softmax2d \
          --launch "$TMP/softmax2d.launch" 2>"$TMP/softmax2d.sim.err" \
          >"$TMP/softmax2d.dump" &&
       python3 - "$TMP/softmax2d.dump" <<'PY'
import struct, sys
words = [int(w) for w in open(sys.argv[1]).read().split()]
got = [struct.unpack('<f', struct.pack('<I', w))[0] for w in words]
xs = [-4.0 + 0.125 * i for i in range(64)]
ok = len(got) == 64
for row in range(8):
    values = xs[row * 8:(row + 1) * 8]
    top = max(values)
    exps = [2.0 ** (v - top) for v in values]
    total = sum(exps)
    for col in range(8):
        ok = ok and abs(got[row * 8 + col] - exps[col] / total) < 1e-5
sys.exit(0 if ok else 1)
PY
    then
      ok "softmax2d matches the base-2 reference and emits exp2 + bar.sync"
    else
      bad "softmax mismatch: $(cat "$TMP/softmax2d.sim.err" 2>/dev/null)"
    fi
  else
    bad "softmax codegen failed: $(cat "$TMP/softmax2d.llc.err" 2>/dev/null)"
  fi
else
  bad "softmax lowering failed: $(cat "$TMP"/softmax2d.t*.err 2>/dev/null)"
fi

# 4. matmul2d: 32^3 on a 4x4 grid of 8x8 tiles, no dot instruction.
if build matmul2d "$ROOT/kernels/matmul2d.py" \
      --arg-type a_ptr=ptr --arg-type b_ptr=ptr --arg-type c_ptr=ptr &&
   grep -q 'tinyttg.get_program_id <axis = 1>' "$TMP/matmul2d.ttgir" &&
   diff -u "$ROOT/tests/passes/matmul2d.llir" "$TMP/matmul2d.llir" \
      >"$TMP/mm.diff"; then
  cat > "$TMP/matmul2d.launch" <<'JSON'
{ "grid_dims": 2, "grid": [4, 4], "block": 32, "args_addr": 16384,
  "args": [0, 4096, 8192],
  "fill": [ { "addr": 0, "start": 0.0, "step": 1.0, "count": 1024, "format": "f32" },
            { "addr": 4096, "start": 1.0, "step": 0.0, "count": 1024, "format": "f32" },
            { "addr": 8192, "start": 0.0, "step": 0.0, "count": 1024, "format": "f32" } ],
  "dump": [ { "addr": 8192, "count": 1024 } ] }
JSON
  if python3 "$SIM" "$TMP/matmul2d.o" --kernel matmul2d \
        --launch "$TMP/matmul2d.launch" 2>"$TMP/mm.sim.err" >"$TMP/mm.dump" &&
     python3 - "$TMP/mm.dump" <<'PY'
import struct, sys
words = [int(w) for w in open(sys.argv[1]).read().split()]
got = [struct.unpack('<f', struct.pack('<I', w))[0] for w in words]
# A[i][k] = 32*i + k and B[k][j] = 1, so C[i][j] = 256*i + 28.
want = [float(256 * (i // 32) + 28) for i in range(1024)]
sys.exit(0 if got == want else 1)
PY
  then
    ok "matmul2d computes a 32x32x32 product on a 2-D grid"
  else
    bad "matmul mismatch: $(cat "$TMP/mm.sim.err" 2>/dev/null; head -c 120 "$TMP/mm.dump" 2>/dev/null)"
  fi
else
  bad "matmul lowering failed: $(cat "$TMP"/matmul2d.t*.err 2>/dev/null; head -20 "$TMP/mm.diff" 2>/dev/null)"
fi

# 5. The forms chapter 7 does not implement are rejected with a reason.
negative_ok=1
check_frontend_error() {
  local name=$1 pattern=$2 py=$3
  shift 3
  if "$FRONTEND" "$py" --emit ttir "$@" -o /dev/null 2>"$TMP/$name.err"; then
    bad "frontend accepted $name"
    negative_ok=0
  elif ! grep -q "$pattern" "$TMP/$name.err"; then
    bad "wrong diagnostic for $name: $(cat "$TMP/$name.err")"
    negative_ok=0
  fi
}

cat > "$TMP/reduce_no_keep.py" <<'PY'
import triton
import triton.language as tl


@triton.jit
def bad(in_ptr):
    rows = tl.arange(0, 8)[:, None]
    cols = tl.arange(0, 8)[None, :]
    x = tl.load(in_ptr + rows * 8 + cols)
    m = tl.max(x, axis=1)
    tl.store(in_ptr, m)
PY
cat > "$TMP/bad_shapes.py" <<'PY'
import triton
import triton.language as tl


@triton.jit
def bad(in_ptr):
    a = tl.arange(0, 8)[None, :]
    b = tl.arange(0, 16)[None, :]
    y = a + b
    tl.store(in_ptr + a, y)
PY
cat > "$TMP/bad_trans.py" <<'PY'
import triton
import triton.language as tl


@triton.jit
def bad(in_ptr):
    rows = tl.arange(0, 8)[:, None]
    cols = tl.arange(0, 8)[None, :]
    x = tl.load(in_ptr + rows)
    tl.store(in_ptr + cols, tl.trans(x))
PY

check_frontend_error reduce_no_keep 'keep_dims=True' "$TMP/reduce_no_keep.py" \
  --arg-type in_ptr=ptr
check_frontend_error bad_shapes 'cannot broadcast dimension' "$TMP/bad_shapes.py" \
  --arg-type in_ptr=ptr
# A transposed dimension of extent 1 is a layout change, not a transpose: it
# only fails once the TTGIR lowering looks at the shapes.
if "$FRONTEND" "$TMP/bad_trans.py" --emit ttir --arg-type in_ptr=ptr \
      -o "$TMP/bad_trans.ttir" 2>"$TMP/bad_trans.err" &&
   "$OPT" --tiny-convert-tt-to-ttg "$TMP/bad_trans.ttir" \
      -o "$TMP/bad_trans.ttgir" 2>>"$TMP/bad_trans.err" &&
   ! "$OPT" --tiny-convert-ttg-to-llvm "$TMP/bad_trans.ttgir" \
      -o /dev/null 2>"$TMP/bad_trans.t5.err"; then
  if ! grep -q 'of extent 1' "$TMP/bad_trans.t5.err"; then
    bad "wrong diagnostic for a 1-wide transpose: $(cat "$TMP/bad_trans.t5.err")"
    negative_ok=0
  fi
else
  bad "a transpose of a 1-wide dimension was accepted"
  negative_ok=0
fi
[ "$negative_ok" = 1 ] && ok "chapter 7 rejects 2-D reductions without keep_dims, bad broadcasts and 1-wide transposes"

echo "----"
echo "chapter 7b: $passed passed, $failed failed"
[ "$failed" -eq 0 ]
