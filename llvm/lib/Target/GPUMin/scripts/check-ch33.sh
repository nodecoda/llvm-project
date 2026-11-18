#!/usr/bin/env bash
#
# Chapter 33 gate: runtime if/else (structured scf.if regions).
#
# Usage: check-ch33.sh <llvm-bin-dir>
#
# What this proves:
#   1. the kernel prints a real scf.if with an arith.cmpi slt condition and an
#      else region, and canonicalises to the golden;
#   2. llc emits a predicated branch plus two exits, and the branch adds no
#      barrier to the ISA surface;
#   3. the simulator takes the then branch for block 0 and the else branch for
#      block 1, bit-exactly;
#   4. an if with no else leaves the untouched block's tile at its fill value;
#   5. reassigning an outer name inside a branch, a tensor condition and a
#      return inside a branch all fail with actionable diagnostics.
#
# TRITON_OPT may point at triton-opt; check 1b is SKIP when it is absent.
#
set -euo pipefail

BIN=${1:?usage: check-ch33.sh <llvm-bin-dir>}
HERE=$(cd "$(dirname "$0")" && pwd)
TOOL="$BIN/gpumin-triton"
LLC="$BIN/llc"
SIM="$HERE/../tools/gpumin_sim.py"
SRC="$HERE/triton/ifelse.py"
GOLDEN="$HERE/triton/ifelse.ttir"
E2E="$HERE/triton/e2e/ifelse"
TMP=$(mktemp -d)
trap 'python3 -c "import shutil,sys; shutil.rmtree(sys.argv[1], ignore_errors=True)" "$TMP"' EXIT
export PYTHONDONTWRITEBYTECODE=1

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

[ -x "$TOOL" ] || fail "missing $TOOL"
[ -x "$LLC" ] || fail "missing $LLC"
[ -f "$SRC" ] || fail "missing $SRC"
[ -f "$GOLDEN" ] || fail "missing $GOLDEN"
[ -f "$E2E/expected.json" ] || fail "missing $E2E/expected.json"

TRITON_OPT=${TRITON_OPT:-$(command -v triton-opt || true)}
EMIT=(--arg-type 'x_ptr=ptr<f32>' --arg-type 'y_ptr=ptr<f32>'
      --arg-type n=i32)

# --- 1. TTIR shape and canonical golden ---------------------------------
"$TOOL" --emit-ttir "${EMIT[@]}" "$SRC" -o "$TMP/ifelse.ttir"
grep -q '^    scf.if %' "$TMP/ifelse.ttir" ||
  fail "the runtime branch did not become a top-level scf.if"
grep -q 'arith.cmpi slt, %pid, %n : i32' "$TMP/ifelse.ttir" ||
  fail "the branch condition is not the expected scalar compare"
grep -q '^    } else {$' "$TMP/ifelse.ttir" ||
  fail "the else region is missing"
[ "$(grep -c 'tt.store' "$TMP/ifelse.ttir")" -eq 2 ] ||
  fail "expected one tt.store in each branch"
if [ -n "$TRITON_OPT" ] && [ -x "$TRITON_OPT" ]; then
  "$TRITON_OPT" "$TMP/ifelse.ttir" -o /dev/null ||
    fail "triton-opt rejected the if/else TTIR"
  "$TRITON_OPT" "$TMP/ifelse.ttir" --canonicalize -o "$TMP/ours.canon.ttir"
  "$TRITON_OPT" "$GOLDEN" --canonicalize -o "$TMP/golden.canon.ttir"
  diff -u "$TMP/golden.canon.ttir" "$TMP/ours.canon.ttir" > "$TMP/canon.diff" ||
    { cat "$TMP/canon.diff" >&2; fail "canonical TTIR differs from the golden"; }
  echo "PASS 1/5: the if/else TTIR canonicalises to the golden"
else
  echo "SKIP 1b/5: triton-opt not found (set TRITON_OPT to enable)"
  echo "PASS 1/5: the if/else TTIR has the expected shape"
fi

# --- 2. codegen: one predicated branch, two exits, no barriers ----------
"$TOOL" --emit-llvm "${EMIT[@]}" "$SRC" -o "$TMP/ifelse.ll"
"$LLC" -march=gpumin -O1 -verify-machineinstrs "$TMP/ifelse.ll" \
  -o "$TMP/ifelse.s" || fail "llc rejected the if/else kernel"
grep -qE '^[[:space:]]+bra p[0-9]+, \.LBB' "$TMP/ifelse.s" ||
  fail "no predicated branch for the runtime if"
[ "$(grep -cE '^[[:space:]]+exit$' "$TMP/ifelse.s")" -ge 2 ] ||
  fail "expected the then and else paths to converge on separate exits"
[ "$(grep -c 'bar.sync' "$TMP/ifelse.s")" -eq 0 ] ||
  fail "a plain if/else must not introduce a barrier"
echo "PASS 2/5: llc emits a predicated branch, two exits and no barrier"

# --- 3. simulator: then for block 0, else for block 1 -------------------
python3 "$HERE/triton/e2e/run_e2e.py" "$E2E" --bin "$BIN" --sim "$SIM" ||
  fail "the if/else kernel mismatched the f32 reference"
echo "PASS 3/5: block 0 takes then and block 1 takes else, bit-exactly"

# --- 4. an if without else leaves the untouched tile alone --------------
mkdir -p "$TMP/noelse"
cat > "$TMP/noelse/kernel.py" <<'PY'
import triton
import triton.language as tl


@triton.jit
def if_no_else_kernel(x_ptr, y_ptr, n):
    pid = tl.program_id(0)
    i = tl.arange(0, 32)
    v = tl.load(x_ptr + pid * 32 + i)
    if pid < n:
        tl.store(y_ptr + pid * 32 + i, v + 1.0)
PY
cat > "$TMP/noelse/launch.json" <<'JSON'
{
  "grid": 3,
  "block": 32,
  "args_addr": 4096,
  "args": [0, 256, 2],
  "fill": [
    { "addr": 0, "start": 0, "step": 1, "count": 96, "format": "f32" },
    { "addr": 256, "start": -7, "step": 0, "count": 96, "format": "f32" }
  ],
  "dump": [
    { "addr": 256, "count": 96 }
  ]
}
JSON
python3 - "$TMP/noelse/expected.json" <<'PY'
import json
import sys

values = [float(i + 1) for i in range(64)] + [-7.0] * 32
doc = {
    "kernel": "if_no_else_kernel",
    "arg_types": {"x_ptr": "ptr<f32>", "y_ptr": "ptr<f32>", "n": "i32"},
    "format": "f32",
    "values": values,
}
open(sys.argv[1], "w").write(json.dumps(doc, indent=2) + "\n")
PY
python3 "$HERE/triton/e2e/run_e2e.py" "$TMP/noelse" --bin "$BIN" --sim "$SIM" ||
  fail "the if-without-else kernel touched a block it should have skipped"
echo "PASS 4/5: an if without else leaves the untouched tile at its fill value"

# --- 5. actionable diagnostics ------------------------------------------
cat > "$TMP/outer.py" <<'PY'
import triton
import triton.language as tl


@triton.jit
def k(p):
    x = tl.load(p)
    if x > 0:
        x = x + 1
    tl.store(p, x)
PY
if "$TOOL" --emit-ttir --arg-type p=ptr "$TMP/outer.py" \
      > /dev/null 2> "$TMP/outer.err"; then
  fail "reassigning an outer name inside a branch was accepted"
fi
grep -q "assignment to 'x' inside if is not supported in chapter 33" \
  "$TMP/outer.err" ||
  { cat "$TMP/outer.err" >&2; fail "the outer-assignment diagnostic is not actionable"; }

cat > "$TMP/tensor.py" <<'PY'
import triton
import triton.language as tl


@triton.jit
def k(p):
    i = tl.arange(0, 32)
    v = tl.load(p + i)
    if v > v:
        tl.store(p + i, v)
PY
if "$TOOL" --emit-ttir --arg-type p=ptr "$TMP/tensor.py" \
      > /dev/null 2> "$TMP/tensor.err"; then
  fail "a tensor condition was accepted"
fi
grep -q 'the if condition must be a scalar i1 in chapter 33' "$TMP/tensor.err" ||
  { cat "$TMP/tensor.err" >&2; fail "the tensor-condition diagnostic is not actionable"; }

cat > "$TMP/early.py" <<'PY'
import triton
import triton.language as tl


@triton.jit
def k(p):
    if tl.program_id(0) < 1:
        return
    tl.store(p, 0.0)
PY
if "$TOOL" --emit-ttir --arg-type p=ptr "$TMP/early.py" \
      > /dev/null 2> "$TMP/early.err"; then
  fail "a return inside a branch was accepted"
fi
grep -q 'return inside if/else is not supported in chapter 33' "$TMP/early.err" ||
  { cat "$TMP/early.err" >&2; fail "the early-return diagnostic is not actionable"; }
echo "PASS 5/5: outer-assignment, tensor-condition and early-return diagnostics are clear"

echo
echo "Chapter 33: 5/5 checks passed"
