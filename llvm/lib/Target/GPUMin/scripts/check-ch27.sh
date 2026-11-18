#!/usr/bin/env bash
#
# Chapter 27 gate: tl.where, compile-time loops and the second-course e2e.
#
# Usage: check-ch27.sh <llvm-bin-dir>
#
# What this proves:
#   1. tl.where lowers to arith.select with an explicit tensor<i1> condition,
#      and triton-opt parses/canonicalises it against the golden;
#   2. tl.static_range and range unroll into constants (no loop in the TTIR),
#      and the unrolled accumulator is 16 on the simulator;
#   3. i32 and f32 selects both reach SELP, and the three kernels run;
#   4. the four e2e programs match their reference JSON;
#   5. while, for/else, oversized ranges and a non-i1 where condition are
#      rejected with actionable diagnostics.
#
# TRITON_OPT may point at triton-opt; check 1b is SKIP when it is absent.
#
set -euo pipefail

BIN=${1:?usage: check-ch27.sh <llvm-bin-dir>}
HERE=$(cd "$(dirname "$0")" && pwd)
TOOL="$BIN/gpumin-triton"
LLC="$BIN/llc"
SIM="$HERE/../tools/gpumin_sim.py"
CF="$HERE/triton/control_flow.py"
GOLDEN="$HERE/triton/control_flow.ttir"
E2E="$HERE/triton/e2e"
TMP=$(mktemp -d)
trap 'python3 -c "import shutil,sys; shutil.rmtree(sys.argv[1], ignore_errors=True)" "$TMP"' EXIT
export PYTHONDONTWRITEBYTECODE=1

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

[ -x "$TOOL" ] || fail "missing $TOOL"
[ -x "$LLC" ] || fail "missing $LLC"
[ -f "$CF" ] || fail "missing $CF"
[ -f "$GOLDEN" ] || fail "missing $GOLDEN"
[ -f "$E2E/run_e2e.py" ] || fail "missing $E2E/run_e2e.py"

TRITON_OPT=${TRITON_OPT:-$(command -v triton-opt || true)}

EMIT=(--constexpr BLOCK=32
      --arg-type x_ptr=ptr --arg-type y_ptr=ptr --arg-type out_ptr=ptr
      --arg-type 'fx_ptr=ptr<f32>' --arg-type 'fy_ptr=ptr<f32>'
      --arg-type 'fout_ptr=ptr<f32>' --arg-type 'sout_ptr=ptr<f32>'
      --arg-type uout_ptr=ptr)

# --- 1. tt.where becomes arith.select with a typed condition ------------
"$TOOL" --emit-ttir "${EMIT[@]}" "$CF" > "$TMP/cf.ttir"
grep -Eq 'arith.select .* : tensor<32xi1>, tensor<32xi32>' "$TMP/cf.ttir" ||
  fail "the i32 where does not print a tensor<i1> condition"
grep -Eq 'arith.select .* : tensor<32xi1>, tensor<32xf32>' "$TMP/cf.ttir" ||
  fail "the f32 where does not print a tensor<i1> condition"
grep -Eq 'arith.select .* : f32' "$TMP/cf.ttir" ||
  fail "the scalar where is not a scalar i1 select"
if [ -n "$TRITON_OPT" ] && [ -x "$TRITON_OPT" ]; then
  "$TRITON_OPT" "$TMP/cf.ttir" -o /dev/null ||
    fail "triton-opt rejected the control-flow TTIR"
  "$TRITON_OPT" "$TMP/cf.ttir" --canonicalize -o "$TMP/ours.canon.ttir"
  "$TRITON_OPT" "$GOLDEN" --canonicalize -o "$TMP/golden.canon.ttir"
  diff -u "$TMP/golden.canon.ttir" "$TMP/ours.canon.ttir" \
    > "$TMP/canon.diff" ||
    { cat "$TMP/canon.diff" >&2; fail "canonical TTIR differs from the golden"; }
  grep -q 'arith.constant 16 : i32' "$TMP/ours.canon.ttir" ||
    fail "the unrolled loops do not fold to 16"
  echo "PASS 1/5: tl.where is a typed arith.select and canonicalises cleanly"
else
  echo "SKIP 1b/5: triton-opt not found (set TRITON_OPT to enable)"
  echo "PASS 1/5: tl.where is a typed arith.select"
fi

# --- 2. static_range/range unroll at compile time ------------------------
for n in 0 1 2 3 4; do
  grep -q "arith.constant $n : i32" "$TMP/cf.ttir" ||
    fail "the unrolled loop is missing the constant $n"
done
if grep -Eq 'scf\.|cf\.br|llvm\.loop' "$TMP/cf.ttir"; then
  fail "a loop survived into the TTIR"
fi
"$TOOL" --emit-llvm "${EMIT[@]}" "$CF" -o "$TMP/cf.ll"
"$LLC" -march=gpumin -O1 -verify-machineinstrs -filetype=obj \
  "$TMP/cf.ll" -o "$TMP/cf.o" || fail "llc rejected the control-flow kernel"
cat > "$TMP/unroll.json" <<'JSON'
{
  "grid": 1,
  "block": 32,
  "args_addr": 4096,
  "args": [0],
  "dump": [ { "addr": 0, "count": 1 } ]
}
JSON
got=$(python3 "$SIM" "$TMP/cf.o" --kernel unroll_kernel \
        --launch "$TMP/unroll.json") ||
  fail "the simulator rejected the unrolled kernel"
[ "$got" = "16" ] || fail "the unrolled accumulator is '$got', want 16"
echo "PASS 2/5: static_range and range unroll to constants and yield 16"

# --- 3. i32/f32 SELP codegen and runtime selects -------------------------
"$LLC" -march=gpumin -O1 -verify-machineinstrs "$TMP/cf.ll" -o "$TMP/cf.s"
grep -q 'fsetp.olt' "$TMP/cf.s" || fail "f32 where did not select fsetp.olt"
[ "$(grep -c 'selp' "$TMP/cf.s")" -ge 2 ] ||
  fail "expected selects for both the scalar and tensor where"

cat > "$TMP/where.json" <<'JSON'
{
  "grid": 1,
  "block": 32,
  "args_addr": 4096,
  "args": [0, 8192, 16384],
  "fill": [
    { "addr": 0, "start": 10, "step": 1, "count": 32 },
    { "addr": 8192, "start": 41, "step": -1, "count": 32 }
  ],
  "dump": [ { "addr": 16384, "count": 8 } ]
}
JSON
got=$(python3 "$SIM" "$TMP/cf.o" --kernel where_kernel --launch "$TMP/where.json") ||
  fail "the simulator rejected the i32 where"
[ "$got" = "10 11 12 13 14 15 16 17" ] ||
  fail "i32 where produced '$got'"

cat > "$TMP/fwhere.json" <<'JSON'
{
  "grid": 1,
  "block": 32,
  "args_addr": 4096,
  "args": [0, 8192, 16384],
  "fill": [
    { "addr": 0, "start": 1069547520, "step": 0, "count": 32 },
    { "addr": 8192, "start": 1074790400, "step": 0, "count": 32 }
  ],
  "dump": [ { "addr": 16384, "count": 2 } ]
}
JSON
got=$(python3 "$SIM" "$TMP/cf.o" --kernel fwhere_kernel --launch "$TMP/fwhere.json") ||
  fail "the simulator rejected the f32 where"
[ "$got" = "1069547520 1069547520" ] ||
  fail "f32 where produced '$got' (want the bit pattern of 1.5)"

cat > "$TMP/swhere.json" <<'JSON'
{
  "grid": 2,
  "block": 32,
  "args_addr": 4096,
  "args": [8192],
  "dump": [ { "addr": 8192, "count": 2 } ]
}
JSON
got=$(python3 "$SIM" "$TMP/cf.o" --kernel swhere_kernel --launch "$TMP/swhere.json") ||
  fail "the simulator rejected the scalar where"
[ "$got" = "1075838976 1069547520" ] ||
  fail "scalar where produced '$got' (want 2.5, 1.5)"
echo "PASS 3/5: i32/f32 where reach SELP and produce the right values"

# --- 4. the four e2e programs -------------------------------------------
for prog in add_i32 add_f32 reduce matmul_no_dot; do
  if ! python3 "$E2E/run_e2e.py" "$E2E/$prog" --bin "$BIN" --sim "$SIM" \
        > "$TMP/e2e-$prog.log" 2>&1; then
    cat "$TMP/e2e-$prog.log" >&2
    fail "e2e program '$prog' failed"
  fi
done
echo "PASS 4/5: add_i32, add_f32, reduce and matmul_no_dot match their references"

# --- 5. actionable diagnostics ------------------------------------------
cat > "$TMP/while.py" <<'PY'
import triton
import triton.language as tl

@triton.jit
def k(p):
    i = 0
    while i < 3:
        i = i + 1
    tl.store(p, i)
PY
if "$TOOL" --emit-ttir --arg-type p=ptr "$TMP/while.py" \
      > "$TMP/while.out" 2> "$TMP/while.err"; then
  fail "while was accepted"
fi
grep -q "unsupported statement 'While'" "$TMP/while.err" ||
  { cat "$TMP/while.err" >&2; fail "while diagnostic is not actionable"; }

cat > "$TMP/orelse.py" <<'PY'
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
if "$TOOL" --emit-ttir --arg-type p=ptr "$TMP/orelse.py" \
      > "$TMP/orelse.out" 2> "$TMP/orelse.err"; then
  fail "for/else was accepted"
fi
grep -q 'for/else is not supported' "$TMP/orelse.err" ||
  { cat "$TMP/orelse.err" >&2; fail "for/else diagnostic is not actionable"; }

cat > "$TMP/toolong.py" <<'PY'
import triton
import triton.language as tl

@triton.jit
def k(p):
    acc = 0
    for i in range(0, 2000):
        acc = acc + i
    tl.store(p, acc)
PY
if "$TOOL" --emit-ttir --arg-type p=ptr "$TMP/toolong.py" \
      > "$TMP/toolong.out" 2> "$TMP/toolong.err"; then
  fail "a 2000-iteration range was accepted"
fi
grep -q 'static range is limited to 1024 iterations' "$TMP/toolong.err" ||
  { cat "$TMP/toolong.err" >&2; fail "range-limit diagnostic is not actionable"; }

cat > "$TMP/badwhere.py" <<'PY'
import triton
import triton.language as tl

@triton.jit
def k(p):
    v = tl.where(1, 2, 3)
    tl.store(p, v)
PY
if "$TOOL" --emit-ttir --arg-type p=ptr "$TMP/badwhere.py" \
      > "$TMP/badwhere.out" 2> "$TMP/badwhere.err"; then
  fail "a non-i1 where condition was accepted"
fi
grep -q 'a scalar tl.where needs an i1 condition' "$TMP/badwhere.err" ||
  { cat "$TMP/badwhere.err" >&2; fail "where diagnostic is not actionable"; }
echo "PASS 5/5: while, for/else, long ranges and bad where conditions are rejected"

echo
echo "Chapter 27: 5/5 checks passed"
