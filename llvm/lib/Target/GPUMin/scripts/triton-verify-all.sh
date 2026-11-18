#!/usr/bin/env bash
#
# Triton frontend acceptance harness (T1-T7).
#
# Usage: triton-verify-all.sh <llvm-bin-dir>
#
# T1  AST: the C++ frontend sees all kernels and their arguments.
# T2  TTIR: every generated module is accepted by triton-opt.
# T3  Canonical: add_i32, f32_add, reduce_sum, control_flow, elementwise,
#     layout2d, matmul2d, softmax2d, attention, ifelse, matmul_shared,
#     dot2d, causal and mask_broadcast match goldens.
# T4  Layout: the Triton layout pass accepts gpumin:1 / 8 threads / 4 warps.
# T5  CodeGen: every e2e kernel compiles with -verify-machineinstrs.
# T6  Simulator: the fourteen e2e programs match their reference JSON.
# T7  Diagnostics: seven unsupported forms (including a 1D tl.dot) fail
#     with actionable messages.
#
# T2-T4 report SKIP when triton-opt is not available; the rest must pass.
#
set -uo pipefail

BIN=${1:?usage: triton-verify-all.sh <llvm-bin-dir>}
HERE=$(cd "$(dirname "$0")" && pwd)
TOOL="$BIN/gpumin-triton"
LLC="$BIN/llc"
SIM="$HERE/../tools/gpumin_sim.py"
TRITON="$HERE/triton"
E2E="$TRITON/e2e"
TMP=$(mktemp -d)
trap 'python3 -c "import shutil,sys; shutil.rmtree(sys.argv[1], ignore_errors=True)" "$TMP"' EXIT
export PYTHONDONTWRITEBYTECODE=1

TRITON_OPT=${TRITON_OPT:-$(command -v triton-opt || true)}

PASSED=0
FAILED=0
SKIPPED=0

pass() { PASSED=$((PASSED + 1)); echo "PASS $1/7: $2"; }
skip() { SKIPPED=$((SKIPPED + 1)); echo "SKIP $1/7: $2"; }
fail() { FAILED=$((FAILED + 1)); echo "FAIL $1/7: $2" >&2; }

[ -x "$TOOL" ] || { echo "missing $TOOL" >&2; exit 1; }
[ -x "$LLC" ] || { echo "missing $LLC" >&2; exit 1; }

# --- T1. AST -------------------------------------------------------------
"$TOOL" --dump-ast "$TRITON/control_flow.py" > "$TMP/ast.txt" 2> "$TMP/ast.err"
if [ "$(grep -c '^kernel ' "$TMP/ast.txt")" -ne 4 ]; then
  cat "$TMP/ast.err" >&2
  fail T1 "AST does not list the control-flow kernels"
else
  "$TOOL" --dump-ast "$E2E/matmul_no_dot/kernel.py" > "$TMP/mm-ast.txt"
  if grep -q '^kernel matmul_kernel ' "$TMP/mm-ast.txt"; then
    pass T1 "the AST lists four control-flow kernels and matmul_kernel"
  else
    fail T1 "matmul_kernel missing from the AST"
  fi
fi

# --- T2. TTIR parse ------------------------------------------------------
if [ -n "$TRITON_OPT" ] && [ -x "$TRITON_OPT" ]; then
  ok=1
  "$TOOL" --emit-ttir --constexpr BLOCK=1024 \
    --arg-type x_ptr=ptr --arg-type y_ptr=ptr --arg-type out_ptr=ptr \
    "$TRITON/add_i32.py" -o "$TMP/add_i32.ttir" || ok=0
  "$TOOL" --emit-ttir --constexpr BLOCK=256 \
    --arg-type 'x_ptr=ptr<f32>' --arg-type 'y_ptr=ptr<f32>' \
    --arg-type 'out_ptr=ptr<f32>' "$TRITON/f32_add.py" -o "$TMP/f32_add.ttir" || ok=0
  "$TOOL" --emit-ttir --constexpr BLOCK=32 \
    --arg-type x_ptr=ptr --arg-type out_ptr=ptr \
    "$TRITON/reduce_sum.py" -o "$TMP/reduce.ttir" || ok=0
  "$TOOL" --emit-ttir --constexpr BLOCK=32 \
    --arg-type x_ptr=ptr --arg-type y_ptr=ptr --arg-type out_ptr=ptr \
    --arg-type 'fx_ptr=ptr<f32>' --arg-type 'fy_ptr=ptr<f32>' \
    --arg-type 'fout_ptr=ptr<f32>' --arg-type 'sout_ptr=ptr<f32>' \
    --arg-type uout_ptr=ptr "$TRITON/control_flow.py" -o "$TMP/cf.ttir" || ok=0
  "$TOOL" --emit-ttir \
    --arg-type 'a_ptr=ptr<f32>' --arg-type 'b_ptr=ptr<f32>' \
    --arg-type 'c_ptr=ptr<f32>' "$E2E/matmul_no_dot/kernel.py" \
    -o "$TMP/matmul.ttir" || ok=0
  "$TOOL" --emit-ttir --constexpr BLOCK=32 --arg-type out_ptr=ptr \
    --arg-type 'fx_ptr=ptr<f32>' --arg-type 'fy_ptr=ptr<f32>' \
    --arg-type 'fout_ptr=ptr<f32>' "$TRITON/elementwise.py" \
    -o "$TMP/elementwise.ttir" || ok=0
  "$TOOL" --emit-ttir --arg-type in_ptr=ptr --arg-type out_ptr=ptr \
    "$TRITON/layout2d.py" -o "$TMP/layout2d.ttir" || ok=0
  "$TOOL" --emit-ttir --arg-type 'a_ptr=ptr<f32>' --arg-type 'b_ptr=ptr<f32>' \
    --arg-type 'c_ptr=ptr<f32>' "$TRITON/matmul2d.py" \
    -o "$TMP/matmul2d.ttir" || ok=0
  "$TOOL" --emit-ttir --arg-type 'x_ptr=ptr<f32>' "$TRITON/softmax2d.py" \
    -o "$TMP/softmax2d.ttir" || ok=0
  "$TOOL" --emit-ttir --arg-type 'q_ptr=ptr<f32>' --arg-type 'k_ptr=ptr<f32>' \
    --arg-type 'v_ptr=ptr<f32>' --arg-type 'p_ptr=ptr<f32>' \
    --arg-type 'o_ptr=ptr<f32>' "$TRITON/attention.py" \
    -o "$TMP/attention.ttir" || ok=0
  "$TOOL" --emit-ttir --arg-type 'x_ptr=ptr<f32>' --arg-type 'y_ptr=ptr<f32>' \
    --arg-type n=i32 "$TRITON/ifelse.py" -o "$TMP/ifelse.ttir" || ok=0
  "$TOOL" --emit-ttir --arg-type 'a_ptr=ptr<f32>' --arg-type 'b_ptr=ptr<f32>' \
    --arg-type 'c_ptr=ptr<f32>' "$TRITON/matmul_shared.py" \
    -o "$TMP/matmul_shared.ttir" || ok=0
  "$TOOL" --emit-ttir --arg-type 'a_ptr=ptr<f32>' --arg-type 'b_ptr=ptr<f32>' \
    --arg-type 'c_ptr=ptr<f32>' "$TRITON/dot2d.py" -o "$TMP/dot2d.ttir" || ok=0
  "$TOOL" --emit-ttir --arg-type 'q_ptr=ptr<f32>' --arg-type 'k_ptr=ptr<f32>' \
    --arg-type 'v_ptr=ptr<f32>' --arg-type 'p_ptr=ptr<f32>' \
    --arg-type 'o_ptr=ptr<f32>' "$TRITON/causal.py" -o "$TMP/causal.ttir" || ok=0
  "$TOOL" --emit-ttir --arg-type 'x_ptr=ptr<f32>' --arg-type 'out_ptr=ptr<f32>' \
    "$TRITON/mask_broadcast.py" -o "$TMP/broadcast.ttir" || ok=0
  for f in add_i32 f32_add reduce cf matmul elementwise layout2d matmul2d \
           softmax2d attention ifelse dot2d causal broadcast; do
    [ -f "$TMP/$f.ttir" ] || ok=0
    if [ -f "$TMP/$f.ttir" ] && ! "$TRITON_OPT" "$TMP/$f.ttir" -o /dev/null; then
      ok=0
    fi
  done
  # Chapter 34 is the one module with unregistered gpumin.* ops.
  if [ -f "$TMP/matmul_shared.ttir" ] &&
     ! "$TRITON_OPT" --allow-unregistered-dialect \
           "$TMP/matmul_shared.ttir" -o /dev/null; then
    ok=0
  fi
  if [ "$ok" = 1 ]; then
    pass T2 "triton-opt parses all fifteen TTIR modules"
  else
    fail T2 "triton-opt rejected a generated module"
  fi
else
  skip T2 "triton-opt not found (set TRITON_OPT to enable)"
fi

# --- T3. canonical goldens ----------------------------------------------
if [ -n "$TRITON_OPT" ] && [ -x "$TRITON_OPT" ]; then
  ok=1
  check_golden() {
    local name=$1 ours=$2 golden=$3
    shift 3
    "$TRITON_OPT" "$@" "$ours" --canonicalize -o "$TMP/$name.ours.ttir" || return 1
    "$TRITON_OPT" "$@" "$golden" --canonicalize -o "$TMP/$name.golden.ttir" ||
      return 1
    diff -u "$TMP/$name.golden.ttir" "$TMP/$name.ours.ttir" \
      > "$TMP/$name.diff" || return 1
  }
  check_golden add_i32 "$TMP/add_i32.ttir" "$TRITON/add_i32.ttir" || ok=0
  check_golden f32_add "$TMP/f32_add.ttir" "$TRITON/f32_add.ttir" || ok=0
  check_golden reduce "$TMP/reduce.ttir" "$TRITON/reduce_sum.ttir" || ok=0
  check_golden control_flow "$TMP/cf.ttir" "$TRITON/control_flow.ttir" || ok=0
  check_golden elementwise "$TMP/elementwise.ttir" \
    "$TRITON/elementwise.ttir" || ok=0
  check_golden layout2d "$TMP/layout2d.ttir" "$TRITON/layout2d.ttir" || ok=0
  check_golden matmul2d "$TMP/matmul2d.ttir" "$TRITON/matmul2d.ttir" || ok=0
  check_golden softmax2d "$TMP/softmax2d.ttir" "$TRITON/softmax2d.ttir" || ok=0
  check_golden attention "$TMP/attention.ttir" "$TRITON/attention.ttir" || ok=0
  check_golden ifelse "$TMP/ifelse.ttir" "$TRITON/ifelse.ttir" || ok=0
  check_golden matmul_shared "$TMP/matmul_shared.ttir" \
    "$TRITON/matmul_shared.ttir" --allow-unregistered-dialect || ok=0
  check_golden dot2d "$TMP/dot2d.ttir" "$TRITON/dot2d.ttir" || ok=0
  check_golden causal "$TMP/causal.ttir" "$TRITON/causal.ttir" || ok=0
  check_golden broadcast "$TMP/broadcast.ttir" \
    "$TRITON/mask_broadcast.ttir" || ok=0
  if [ "$ok" = 1 ]; then
    pass T3 "all fourteen canonical forms match their goldens"
  else
    for d in "$TMP"/*.diff; do
      [ -f "$d" ] && cat "$d" >&2
    done
    fail T3 "a canonical form differs from its golden"
  fi
else
  skip T3 "triton-opt not found (set TRITON_OPT to enable)"
fi

# --- T4. layout pass -----------------------------------------------------
if [ -n "$TRITON_OPT" ] && [ -x "$TRITON_OPT" ]; then
  ok=1
  for f in "$TMP/add_i32.ttir" "$TMP/cf.ttir"; do
    out="$TMP/layout-$(basename "$f")"
    "$TRITON_OPT" "$f" \
      --convert-triton-to-tritongpu='target=gpumin:1 num-warps=4 threads-per-warp=8' \
      -o "$out" || { ok=0; continue; }
    grep -q 'ttg.target = "gpumin:1"' "$out" || ok=0
    grep -q 'threadsPerWarp = \[8\]' "$out" || ok=0
    grep -q 'warpsPerCTA = \[4\]' "$out" || ok=0
  done
  if [ "$ok" = 1 ]; then
    pass T4 "the layout pass maps to gpumin:1 with 8x4 threads"
  else
    fail T4 "layout conversion is wrong"
  fi
else
  skip T4 "triton-opt not found (set TRITON_OPT to enable)"
fi

# --- T5. codegen ---------------------------------------------------------
"$TOOL" --emit-llvm --constexpr BLOCK=256 \
  --arg-type x_ptr=ptr --arg-type y_ptr=ptr --arg-type out_ptr=ptr \
  "$TRITON/add_i32.py" -o "$TMP/add_i32.ll"
"$TOOL" --emit-llvm --constexpr BLOCK=64 \
  --arg-type 'x_ptr=ptr<f32>' --arg-type 'y_ptr=ptr<f32>' \
  --arg-type 'out_ptr=ptr<f32>' "$TRITON/f32_add.py" -o "$TMP/f32_add.ll"
"$TOOL" --emit-llvm --constexpr BLOCK=32 \
  --arg-type x_ptr=ptr --arg-type out_ptr=ptr \
  "$TRITON/reduce_sum.py" -o "$TMP/reduce.ll"
"$TOOL" --emit-llvm \
  --arg-type 'a_ptr=ptr<f32>' --arg-type 'b_ptr=ptr<f32>' \
  --arg-type 'c_ptr=ptr<f32>' "$E2E/matmul_no_dot/kernel.py" \
  -o "$TMP/matmul.ll"
"$TOOL" --emit-llvm --constexpr BLOCK=32 \
  --arg-type x_ptr=ptr --arg-type y_ptr=ptr --arg-type out_ptr=ptr \
  --arg-type 'fx_ptr=ptr<f32>' --arg-type 'fy_ptr=ptr<f32>' \
  --arg-type 'fout_ptr=ptr<f32>' --arg-type 'sout_ptr=ptr<f32>' \
  --arg-type uout_ptr=ptr "$TRITON/control_flow.py" -o "$TMP/cf.ll"
"$TOOL" --emit-llvm --constexpr BLOCK=32 --arg-type out_ptr=ptr \
  --arg-type 'fx_ptr=ptr<f32>' --arg-type 'fy_ptr=ptr<f32>' \
  --arg-type 'fout_ptr=ptr<f32>' "$TRITON/elementwise.py" \
  -o "$TMP/elementwise.ll"
"$TOOL" --emit-llvm --arg-type in_ptr=ptr --arg-type out_ptr=ptr \
  "$TRITON/layout2d.py" -o "$TMP/layout2d.ll"
"$TOOL" --emit-llvm --arg-type 'a_ptr=ptr<f32>' --arg-type 'b_ptr=ptr<f32>' \
  --arg-type 'c_ptr=ptr<f32>' "$TRITON/matmul2d.py" -o "$TMP/matmul2d.ll"
"$TOOL" --emit-llvm --arg-type 'x_ptr=ptr<f32>' "$TRITON/softmax2d.py" \
  -o "$TMP/softmax2d.ll"
"$TOOL" --emit-llvm --arg-type 'q_ptr=ptr<f32>' --arg-type 'k_ptr=ptr<f32>' \
  --arg-type 'v_ptr=ptr<f32>' --arg-type 'p_ptr=ptr<f32>' \
  --arg-type 'o_ptr=ptr<f32>' "$TRITON/attention.py" -o "$TMP/attention.ll"
"$TOOL" --emit-llvm --arg-type 'x_ptr=ptr<f32>' --arg-type 'y_ptr=ptr<f32>' \
  --arg-type n=i32 "$TRITON/ifelse.py" -o "$TMP/ifelse.ll"
"$TOOL" --emit-llvm --arg-type 'a_ptr=ptr<f32>' --arg-type 'b_ptr=ptr<f32>' \
  --arg-type 'c_ptr=ptr<f32>' "$TRITON/matmul_shared.py" \
  -o "$TMP/matmul_shared.ll"
"$TOOL" --emit-llvm --arg-type 'a_ptr=ptr<f32>' --arg-type 'b_ptr=ptr<f32>' \
  --arg-type 'c_ptr=ptr<f32>' "$TRITON/dot2d.py" -o "$TMP/dot2d.ll"
"$TOOL" --emit-llvm --arg-type 'q_ptr=ptr<f32>' --arg-type 'k_ptr=ptr<f32>' \
  --arg-type 'v_ptr=ptr<f32>' --arg-type 'p_ptr=ptr<f32>' \
  --arg-type 'o_ptr=ptr<f32>' "$TRITON/causal.py" -o "$TMP/causal.ll"
"$TOOL" --emit-llvm --arg-type 'x_ptr=ptr<f32>' --arg-type 'out_ptr=ptr<f32>' \
  "$TRITON/mask_broadcast.py" -o "$TMP/broadcast.ll"
ok=1
for f in add_i32 f32_add reduce matmul cf elementwise layout2d matmul2d \
         softmax2d attention ifelse matmul_shared dot2d causal broadcast; do
  "$LLC" -march=gpumin -O1 -verify-machineinstrs -filetype=obj \
    "$TMP/$f.ll" -o "$TMP/$f.o" || ok=0
done
if [ "$ok" = 1 ]; then
  pass T5 "all fifteen kernels compile with -verify-machineinstrs"
else
  fail T5 "llc rejected a generated kernel"
fi

# --- T6. simulator e2e ---------------------------------------------------
ok=1
for prog in add_i32 add_f32 reduce matmul_no_dot elementwise layout2d \
            matmul2d softmax2d attention ifelse matmul_shared dot2d causal \
            mask_broadcast; do
  python3 "$E2E/run_e2e.py" "$E2E/$prog" --bin "$BIN" --sim "$SIM" \
    > "$TMP/e2e-$prog.log" 2>&1 || { cat "$TMP/e2e-$prog.log" >&2; ok=0; }
done
if [ "$ok" = 1 ]; then
  pass T6 "the fourteen e2e programs match their reference JSON"
else
  fail T6 "an e2e program mismatched"
fi

# --- T7. diagnostics -----------------------------------------------------
cat > "$TMP/neg-while.py" <<'PY'
import triton
import triton.language as tl

@triton.jit
def k(p):
    i = 0
    while i < 3:
        i = i + 1
    tl.store(p, i)
PY
cat > "$TMP/neg-orelse.py" <<'PY'
import triton
import triton.language as tl

@triton.jit
def k(p):
    for i in range(0, 2):
        p = p + i
    else:
        p = p + 1
    tl.store(p, 0)
PY
cat > "$TMP/neg-long.py" <<'PY'
import triton
import triton.language as tl

@triton.jit
def k(p):
    acc = 0
    for i in range(0, 2000):
        acc = acc + i
    tl.store(p, acc)
PY
cat > "$TMP/neg-where.py" <<'PY'
import triton
import triton.language as tl

@triton.jit
def k(p):
    v = tl.where(1, 2, 3)
    tl.store(p, v)
PY
cat > "$TMP/neg-maxshape.py" <<'PY'
import triton
import triton.language as tl

@triton.jit
def k(p):
    a = tl.arange(0, 32)
    b = tl.arange(0, 64)
    v = tl.maximum(a, b)
    tl.store(p, v)
PY
cat > "$TMP/neg-reduce2d.py" <<'PY'
import triton
import triton.language as tl

@triton.jit
def k(p):
    r = tl.arange(0, 8)[:, None]
    c = tl.arange(0, 8)[None, :]
    x = tl.load(p + r * 8 + c)
    v = tl.max(x, axis=0)
    tl.store(p, v)
PY
cat > "$TMP/neg-dot.py" <<'PY'
import triton
import triton.language as tl

@triton.jit
def k(p):
    x = tl.load(p + tl.arange(0, 8))
    y = tl.load(p + tl.arange(0, 8))
    z = tl.dot(x, y)
    tl.store(p + tl.arange(0, 8), z)
PY
expect_fail() {
  local what=$1 pattern=$2 file=$3
  if "$TOOL" --emit-ttir --arg-type p=ptr "$file" \
        > /dev/null 2> "$TMP/neg.err"; then
    fail T7 "$what was accepted"
    return 1
  fi
  if ! grep -q "$pattern" "$TMP/neg.err"; then
    cat "$TMP/neg.err" >&2
    fail T7 "$what has no actionable diagnostic"
    return 1
  fi
  return 0
}
ok=1
expect_fail "while" "unsupported statement 'While'" "$TMP/neg-while.py" || ok=0
expect_fail "for/else" 'for/else is not supported' "$TMP/neg-orelse.py" || ok=0
expect_fail "a 2000-iteration range" \
  'static range is limited to 1024 iterations' "$TMP/neg-long.py" || ok=0
expect_fail "a non-i1 where condition" \
  'a scalar tl.where needs an i1 condition' "$TMP/neg-where.py" || ok=0
expect_fail "a shape-mismatched maximum" \
  'tensor type mismatch' "$TMP/neg-maxshape.py" || ok=0
expect_fail "a 2D reduction without keep_dims" \
  'keep_dims=True' "$TMP/neg-reduce2d.py" || ok=0
expect_fail "a 1D tl.dot" \
  'tl.dot requires 2D f32 tiles' "$TMP/neg-dot.py" || ok=0
if [ "$ok" = 1 ]; then
  pass T7 "seven unsupported forms fail with actionable messages"
fi

echo
echo "Triton gates: $PASSED passed, $FAILED failed, $SKIPPED skipped"
[ "$FAILED" -eq 0 ]
