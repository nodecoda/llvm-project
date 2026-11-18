#!/usr/bin/env bash
#
# Chapter 35 gate: tl.dot.
#
# Usage: check-ch35.sh <llvm-bin-dir>
#
# What this proves:
#   1. the whole chapter-30 K loop is now one real tt.dot with
#      inputPrecision = tf32, parseable by the stock triton-opt, and it
#      canonicalises to the golden;
#   2. llc expands it with no new ISA: shared staging plus a rank-1 loop, so
#      the asm has 64 fmul / 64 fadd, shared loads and exactly one bar.sync,
#      and no MMA instruction;
#   3. the simulator reproduces chapter 30's 1024 outputs bit for bit under
#      the same launch descriptor;
#   4. the TTIR is structurally smaller than chapter 34's hand-written loop
#      (0 arith.mulf vs 64) while global loads stay identical;
#   5. rank, K, acc shape, dtype and per-thread element violations all fail
#      with actionable diagnostics.
#
# TRITON_OPT may point at triton-opt; check 1b is SKIP when it is absent.
#
set -euo pipefail

BIN=${1:?usage: check-ch35.sh <llvm-bin-dir>}
HERE=$(cd "$(dirname "$0")" && pwd)
TOOL="$BIN/gpumin-triton"
LLC="$BIN/llc"
SIM="$HERE/../tools/gpumin_sim.py"
SRC="$HERE/triton/dot2d.py"
GOLDEN="$HERE/triton/dot2d.ttir"
E2E="$HERE/triton/e2e/dot2d"
RANK1="$HERE/triton/e2e/matmul2d"
CH34="$HERE/triton/e2e/matmul_shared"
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
EMIT=(--arg-type 'a_ptr=ptr<f32>' --arg-type 'b_ptr=ptr<f32>'
      --arg-type 'c_ptr=ptr<f32>')

# --- 1. TTIR shape and canonical golden ---------------------------------
"$TOOL" --emit-ttir "${EMIT[@]}" "$SRC" -o "$TMP/dot.ttir"
[ "$(grep -c 'tt\.dot' "$TMP/dot.ttir")" -eq 1 ] ||
  fail "expected exactly one tt.dot"
grep -q 'tt.dot .*inputPrecision = tf32' "$TMP/dot.ttir" ||
  fail "the dot does not carry inputPrecision = tf32"
grep -q 'tensor<8x32xf32> \* tensor<32x8xf32> -> tensor<8x8xf32>' \
  "$TMP/dot.ttir" || fail "the dot operand/result types are wrong"
[ "$(grep -c 'arith\.mulf' "$TMP/dot.ttir")" -eq 0 ] ||
  fail "the K loop did not fold into tt.dot"
if [ -n "$TRITON_OPT" ] && [ -x "$TRITON_OPT" ]; then
  "$TRITON_OPT" "$TMP/dot.ttir" -o /dev/null ||
    fail "triton-opt rejected the dot TTIR"
  "$TRITON_OPT" "$TMP/dot.ttir" --canonicalize -o "$TMP/ours.canon.ttir"
  "$TRITON_OPT" "$GOLDEN" --canonicalize -o "$TMP/golden.canon.ttir"
  diff -u "$TMP/golden.canon.ttir" "$TMP/ours.canon.ttir" > "$TMP/canon.diff" ||
    { cat "$TMP/canon.diff" >&2; fail "canonical TTIR differs from the golden"; }
  echo "PASS 1/5: one tt.dot canonicalises to the golden"
else
  echo "SKIP 1b/5: triton-opt not found (set TRITON_OPT to enable)"
  echo "PASS 1/5: the dot TTIR has the expected shape"
fi

# --- 2. codegen expands the dot with no new ISA -------------------------
"$TOOL" --emit-llvm "${EMIT[@]}" "$SRC" -o "$TMP/dot.ll"
"$LLC" -march=gpumin -O1 -verify-machineinstrs "$TMP/dot.ll" \
  -o "$TMP/dot.s" || fail "llc rejected the dot kernel"
[ "$(grep -cE '^[[:space:]]+fmul' "$TMP/dot.s")" -eq 64 ] ||
  fail "expected 2 elements/thread x 32 steps fmuls"
[ "$(grep -cE '^[[:space:]]+fadd' "$TMP/dot.s")" -eq 64 ] ||
  fail "expected 2 elements/thread x 32 steps fadds"
[ "$(grep -c 'st\.shared\.b32' "$TMP/dot.s")" -eq 16 ] ||
  fail "the two staged tiles should store 8 elements per thread each"
[ "$(grep -c 'ld\.shared\.b32' "$TMP/dot.s")" -eq 96 ] ||
  fail "expected 32 steps x (2 column + 1 row) slice loads"
[ "$(grep -c 'bar\.sync' "$TMP/dot.s")" -eq 1 ] ||
  fail "dot staging must add exactly one barrier"
if grep -q 'mma' "$TMP/dot.s"; then
  fail "the dot must not introduce an MMA opcode"
fi
echo "PASS 2/5: one bar.sync and 64 fmul/fadd expand the dot, no new ISA"

# --- 3. simulator: chapter 30's numbers, bit for bit --------------------
python3 "$HERE/triton/e2e/run_e2e.py" "$E2E" --bin "$BIN" --sim "$SIM" ||
  fail "the dot kernel mismatched chapter 30"
if ! python3 - "$RANK1/expected.json" "$E2E/expected.json" <<'PY'
import json
import sys

rank1 = json.load(open(sys.argv[1]))
dot = json.load(open(sys.argv[2]))
if rank1["values"] != dot["values"]:
    print("the dot expected.json is not a copy of matmul2d", file=sys.stderr)
    sys.exit(1)
PY
then
  fail "the chapter-35 reference values drifted from chapter 30"
fi
echo "PASS 3/5: the 1024 outputs match chapter 30 and the reference is shared"

# --- 4. the TTIR shrank, the traffic did not ----------------------------
"$TOOL" --emit-ttir "${EMIT[@]}" "$CH34/kernel.py" -o "$TMP/shared.ttir"
[ "$(grep -c 'arith\.mulf' "$TMP/shared.ttir")" -ge 32 ] ||
  fail "chapter 34's kernel should still spell out the K loop"
if ! python3 - "$BIN" "$SIM" "$CH34" "$E2E" "$RANK1" <<'PY'
import json
import subprocess
import sys
import tempfile
from pathlib import Path

binp, sim, ch34, dot, rank1 = (sys.argv[1], sys.argv[2], Path(sys.argv[3]),
                               Path(sys.argv[4]), Path(sys.argv[5]))


def stats(pdir):
    doc = json.loads((pdir / "expected.json").read_text())
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        emit = [str(Path(binp) / "gpumin-triton"), "--emit-llvm"]
        for name, ty in doc["arg_types"].items():
            emit += ["--arg-type", f"{name}={ty}"]
        emit += [str(pdir / "kernel.py"), "-o", str(tmp / "k.ll")]
        subprocess.run(emit, check=True)
        subprocess.run(
            [str(Path(binp) / "llc"), "-march=gpumin", "-O1",
             "-verify-machineinstrs", "-filetype=obj", str(tmp / "k.ll"),
             "-o", str(tmp / "k.o")], check=True)
        run = subprocess.run(
            [sys.executable, sim, str(tmp / "k.o"), "--kernel", doc["kernel"],
             "--launch", str(rank1 / "launch.json"), "--stats"],
            capture_output=True, text=True, check=True)
        out = {}
        for token in run.stderr.split():
            if "=" in token:
                key, value = token.split("=", 1)
                out[key] = int(value)
        return out


before = stats(ch34)
after = stats(dot)
print(f"ch34 loop: issued={before['issued']} cycles={before['cycles']} "
      f"loc_st={before['loc_st']} glb_ld={before['glb_ld']}")
print(f"ch35 dot:  issued={after['issued']} cycles={after['cycles']} "
      f"loc_st={after['loc_st']} glb_ld={after['glb_ld']}")
if after["glb_ld"] != before["glb_ld"]:
    print("the dot changed the staging traffic", file=sys.stderr)
    sys.exit(1)
if after["shm_ld"] <= 0:
    print("the dot never reads shared memory", file=sys.stderr)
    sys.exit(1)
PY
then
  fail "the dot structural/statistics comparison failed"
fi
echo "PASS 4/5: the TTIR has no K loop left and global loads are unchanged"

# --- 5. actionable diagnostics ------------------------------------------
cat > "$TMP/rank.py" <<'PY'
import triton
import triton.language as tl


@triton.jit
def k(p):
    a = tl.load(p + tl.arange(0, 8))
    c = tl.dot(a, a)
    tl.store(p, c)
PY
if "$TOOL" --emit-ttir --arg-type p=ptr "$TMP/rank.py" \
      > /dev/null 2> "$TMP/rank.err"; then
  fail "a 1D dot was accepted"
fi
grep -q 'tl.dot requires 2D f32 tiles' "$TMP/rank.err" ||
  { cat "$TMP/rank.err" >&2; fail "the rank diagnostic is not actionable"; }

cat > "$TMP/kmismatch.py" <<'PY'
import triton
import triton.language as tl


@triton.jit
def k(p):
    r8 = tl.arange(0, 8)[:, None]
    c8 = tl.arange(0, 8)[None, :]
    r16 = tl.arange(0, 16)[:, None]
    a = tl.load(p + r8 * 16 + c8)   # [8,8]
    b = tl.load(p + r16 * 8 + c8)   # [16,8]
    d = tl.dot(a, b)
    tl.store(p + r8 * 8 + c8, d)
PY
if "$TOOL" --emit-ttir --arg-type 'p=ptr<f32>' "$TMP/kmismatch.py" \
      > /dev/null 2> "$TMP/kmismatch.err"; then
  fail "a K mismatch was accepted"
fi
grep -q 'tl.dot K dimensions do not match' "$TMP/kmismatch.err" ||
  { cat "$TMP/kmismatch.err" >&2; fail "the K diagnostic is not actionable"; }

cat > "$TMP/badacc.py" <<'PY'
import triton
import triton.language as tl


@triton.jit
def k(p):
    r = tl.arange(0, 8)[:, None]
    c = tl.arange(0, 8)[None, :]
    a = tl.load(p + r * 8 + c)
    acc = tl.zeros((8, 4), tl.float32)
    d = tl.dot(a, a, acc)
    tl.store(p, d)
PY
if "$TOOL" --emit-ttir --arg-type 'p=ptr<f32>' "$TMP/badacc.py" \
      > /dev/null 2> "$TMP/badacc.err"; then
  fail "an acc shape mismatch was accepted"
fi
grep -q 'tl.dot acc shape does not match a\*b' "$TMP/badacc.err" ||
  { cat "$TMP/badacc.err" >&2; fail "the acc diagnostic is not actionable"; }

cat > "$TMP/i32.py" <<'PY'
import triton
import triton.language as tl


@triton.jit
def k(p):
    r = tl.arange(0, 8)[:, None]
    c = tl.arange(0, 8)[None, :]
    a = tl.load(p + r * 8 + c)
    d = tl.dot(a, a)
    tl.store(p, d)
PY
if "$TOOL" --emit-ttir --arg-type p=ptr "$TMP/i32.py" \
      > /dev/null 2> "$TMP/i32.err"; then
  fail "an i32 dot was accepted"
fi
grep -q 'tl.dot requires 2D f32 tiles' "$TMP/i32.err" ||
  { cat "$TMP/i32.err" >&2; fail "the dtype diagnostic is not actionable"; }

cat > "$TMP/toobig.py" <<'PY'
import triton
import triton.language as tl


@triton.jit
def k(p):
    r = tl.arange(0, 16)[:, None]
    c = tl.arange(0, 32)[None, :]
    a = tl.load(p + r * 32 + c)
    d = tl.dot(a, a)
    tl.store(p, d)
PY
if "$TOOL" --emit-ttir --arg-type 'p=ptr<f32>' "$TMP/toobig.py" \
      > /dev/null 2> "$TMP/toobig.err"; then
  fail "a 16-elements-per-thread dot was accepted"
fi
grep -q 'at most 8 elements per thread' "$TMP/toobig.err" ||
  { cat "$TMP/toobig.err" >&2; fail "the element diagnostic is not actionable"; }
echo "PASS 5/5: rank, K, acc, dtype and element-count diagnostics are clear"

echo
echo "Chapter 35: 5/5 checks passed"
