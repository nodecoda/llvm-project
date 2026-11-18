#!/usr/bin/env bash
#
# Chapter 34 gate: shared-memory staging and column/row slices.
#
# Usage: check-ch34.sh <llvm-bin-dir>
#
# What this proves:
#   1. the kernel prints two gpumin.shared_alloc ops, two shared_store ops and
#      64 indexed shared_col/shared_row ops, and canonicalises to the golden
#      (with --allow-unregistered-dialect);
#   2. llc emits sr.shared, st.shared.b32 and ld.shared.b32, and exactly one
#      bar.sync sits between the last staging store and the first slice load;
#   3. the shared-tiled matmul reproduces chapter 30's 1024 outputs bit for
#      bit under the same launch descriptor;
#   4. global loads drop by at least 2x on that launch while shared loads
#      become non-zero;
#   5. shape mismatch, a runtime slice index, an out-of-range index and
#      arithmetic on a handle all fail with actionable diagnostics.
#
# TRITON_OPT may point at triton-opt; check 1b is SKIP when it is absent.
#
set -euo pipefail

BIN=${1:?usage: check-ch34.sh <llvm-bin-dir>}
HERE=$(cd "$(dirname "$0")" && pwd)
TOOL="$BIN/gpumin-triton"
LLC="$BIN/llc"
SIM="$HERE/../tools/gpumin_sim.py"
SRC="$HERE/triton/matmul_shared.py"
GOLDEN="$HERE/triton/matmul_shared.ttir"
E2E="$HERE/triton/e2e/matmul_shared"
RANK1="$HERE/triton/e2e/matmul2d"
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
[ -f "$RANK1/expected.json" ] || fail "missing $RANK1/expected.json"

TRITON_OPT=${TRITON_OPT:-$(command -v triton-opt || true)}
EMIT=(--arg-type 'a_ptr=ptr<f32>' --arg-type 'b_ptr=ptr<f32>'
      --arg-type 'c_ptr=ptr<f32>')

# --- 1. TTIR shape and canonical golden ---------------------------------
"$TOOL" --emit-ttir "${EMIT[@]}" "$SRC" -o "$TMP/ms.ttir"
[ "$(grep -c '"gpumin.shared_alloc"' "$TMP/ms.ttir")" -eq 2 ] ||
  fail "expected two shared allocations"
[ "$(grep -c '"gpumin.shared_store"' "$TMP/ms.ttir")" -eq 2 ] ||
  fail "expected two shared stores"
[ "$(grep -c '"gpumin.shared_col"' "$TMP/ms.ttir")" -eq 32 ] ||
  fail "expected 32 shared column slices"
[ "$(grep -c '"gpumin.shared_row"' "$TMP/ms.ttir")" -eq 32 ] ||
  fail "expected 32 shared row slices"
grep -q 'gpumin.shared_col.*index = 0 : i32' "$TMP/ms.ttir" ||
  fail "the first column index is not 0"
grep -q 'gpumin.shared_col.*index = 31 : i32' "$TMP/ms.ttir" ||
  fail "the last column index is not 31"
if [ -n "$TRITON_OPT" ] && [ -x "$TRITON_OPT" ]; then
  "$TRITON_OPT" --allow-unregistered-dialect "$TMP/ms.ttir" -o /dev/null ||
    fail "triton-opt rejected the shared-tile TTIR"
  "$TRITON_OPT" --allow-unregistered-dialect --canonicalize \
    "$TMP/ms.ttir" -o "$TMP/ours.canon.ttir"
  "$TRITON_OPT" --allow-unregistered-dialect --canonicalize \
    "$GOLDEN" -o "$TMP/golden.canon.ttir"
  diff -u "$TMP/golden.canon.ttir" "$TMP/ours.canon.ttir" > "$TMP/canon.diff" ||
    { cat "$TMP/canon.diff" >&2; fail "canonical TTIR differs from the golden"; }
  echo "PASS 1/5: the shared-tile TTIR canonicalises to the golden"
else
  echo "SKIP 1b/5: triton-opt not found (set TRITON_OPT to enable)"
  echo "PASS 1/5: the shared-tile TTIR has the expected shape"
fi

# --- 2. codegen: one barrier between staging and slices -----------------
"$TOOL" --emit-llvm "${EMIT[@]}" "$SRC" -o "$TMP/ms.ll"
"$LLC" -march=gpumin -O1 -verify-machineinstrs "$TMP/ms.ll" \
  -o "$TMP/ms.s" || fail "llc rejected the shared-tile kernel"
grep -q 'sr.shared' "$TMP/ms.s" || fail "the shared window does not use sr.shared"
[ "$(grep -c 'st\.shared\.b32' "$TMP/ms.s")" -eq 16 ] ||
  fail "expected 8 elements per thread for each of the two staged strips"
[ "$(grep -c 'ld\.shared\.b32' "$TMP/ms.s")" -eq 96 ] ||
  fail "expected 32 iterations x (2 column + 1 row) slice loads"
[ "$(grep -c 'bar\.sync' "$TMP/ms.s")" -eq 1 ] ||
  fail "staging must add exactly one barrier"
last_st=$(grep -n 'st\.shared' "$TMP/ms.s" | tail -1 | cut -d: -f1)
first_ld=$(grep -n 'ld\.shared' "$TMP/ms.s" | head -1 | cut -d: -f1)
bar=$(grep -n 'bar\.sync' "$TMP/ms.s" | head -1 | cut -d: -f1)
[ "$bar" -gt "$last_st" ] || fail "the barrier is before the last staging store"
[ "$bar" -lt "$first_ld" ] || fail "the barrier is after the first slice load"
echo "PASS 2/5: one bar.sync separates 16 shared stores from 96 slice loads"

# --- 3. simulator: chapter 30's numbers, bit for bit --------------------
python3 "$HERE/triton/e2e/run_e2e.py" "$E2E" --bin "$BIN" --sim "$SIM" ||
  fail "the shared-tiled matmul mismatched chapter 30"
if ! python3 - "$RANK1/expected.json" "$E2E/expected.json" <<'PY'
import json
import sys

rank1 = json.load(open(sys.argv[1]))
shared = json.load(open(sys.argv[2]))
if rank1["values"] != shared["values"]:
    print("the shared-tile expected.json is not a copy of matmul2d", file=sys.stderr)
    sys.exit(1)
PY
then
  fail "the chapter-34 reference values drifted from chapter 30"
fi
echo "PASS 3/5: the 1024 outputs match chapter 30 and the reference is shared"

# --- 4. the measurable win: fewer global loads --------------------------
if ! python3 - "$BIN" "$SIM" "$RANK1" "$E2E" <<'PY'
import json
import subprocess
import sys
import tempfile
from pathlib import Path

binp, sim, rank1, shared = sys.argv[1], sys.argv[2], Path(sys.argv[3]), Path(sys.argv[4])


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


before = stats(rank1)
after = stats(shared)
print(f"rank-1: glb_ld={before['glb_ld']} shm_ld={before['shm_ld']} "
      f"loc_st={before['loc_st']}")
print(f"shared: glb_ld={after['glb_ld']} shm_ld={after['shm_ld']} "
      f"loc_st={after['loc_st']}")
if after["glb_ld"] * 2 > before["glb_ld"]:
    print("shared staging did not at least halve global loads", file=sys.stderr)
    sys.exit(1)
if after["shm_ld"] <= before["shm_ld"]:
    print("the slice loads never touch shared memory", file=sys.stderr)
    sys.exit(1)
PY
then
  fail "shared staging did not halve global loads"
fi
echo "PASS 4/5: shared staging cuts global loads by more than 2x"

# --- 5. actionable diagnostics ------------------------------------------
cat > "$TMP/badshape.py" <<'PY'
import triton
import triton.language as tl


@triton.jit
def k(p):
    sa = tl.alloc_shared((8, 8), tl.float32)
    v = tl.load(p + tl.arange(0, 32))
    tl.store_shared(sa, v)
PY
if "$TOOL" --emit-ttir --arg-type p=ptr "$TMP/badshape.py" \
      > /dev/null 2> "$TMP/badshape.err"; then
  fail "a mismatched store_shared was accepted"
fi
grep -q 'source shape does not match the shared tile' "$TMP/badshape.err" ||
  { cat "$TMP/badshape.err" >&2; fail "the shape diagnostic is not actionable"; }

cat > "$TMP/runtimeidx.py" <<'PY'
import triton
import triton.language as tl


@triton.jit
def k(p):
    sa = tl.alloc_shared((8, 8), tl.float32)
    i = tl.program_id(0)
    v = tl.load_shared_col(sa, i)
    tl.store(p, v)
PY
if "$TOOL" --emit-ttir --arg-type p=ptr "$TMP/runtimeidx.py" \
      > /dev/null 2> "$TMP/runtimeidx.err"; then
  fail "a runtime slice index was accepted"
fi
grep -q 'is not a compile-time integer' "$TMP/runtimeidx.err" ||
  { cat "$TMP/runtimeidx.err" >&2; fail "the index diagnostic is not actionable"; }

cat > "$TMP/badindex.py" <<'PY'
import triton
import triton.language as tl


@triton.jit
def k(p):
    sa = tl.alloc_shared((8, 8), tl.float32)
    v = tl.load_shared_col(sa, 8)
    tl.store(p, v)
PY
if "$TOOL" --emit-ttir --arg-type p=ptr "$TMP/badindex.py" \
      > /dev/null 2> "$TMP/badindex.err"; then
  fail "an out-of-range shared index was accepted"
fi
grep -q 'the shared column index is out of range' "$TMP/badindex.err" ||
  { cat "$TMP/badindex.err" >&2; fail "the range diagnostic is not actionable"; }

cat > "$TMP/handlemath.py" <<'PY'
import triton
import triton.language as tl


@triton.jit
def k(p):
    sa = tl.alloc_shared((8, 8), tl.float32)
    v = sa + 1
    tl.store(p, v)
PY
if "$TOOL" --emit-ttir --arg-type p=ptr "$TMP/handlemath.py" \
      > /dev/null 2> "$TMP/handlemath.err"; then
  fail "arithmetic on a shared handle was accepted"
fi
grep -q 'a shared tile handle cannot be used in arithmetic' \
  "$TMP/handlemath.err" ||
  { cat "$TMP/handlemath.err" >&2; fail "the handle diagnostic is not actionable"; }
echo "PASS 5/5: shape, runtime-index, range and handle diagnostics are clear"

echo
echo "Chapter 34: 5/5 checks passed"
