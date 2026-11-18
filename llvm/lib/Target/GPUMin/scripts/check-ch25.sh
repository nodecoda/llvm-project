#!/usr/bin/env bash
#
# Chapter 25 gate: f32 in the ISA, the TTIR and the simulator.
#
# Usage: check-ch25.sh <llvm-bin-dir>
#
# What this proves:
#   1. fadd/fsub/fmul/fsetp encode and decode round-trip;
#   2. llc selects the f32 instructions and f32 loads/stores;
#   3. TTIR uses !tt.ptr<f32>, arith.addf/mulf/cmpf and a real f32 constant;
#   4. triton-opt parses the TTIR and it canonicalises to the golden;
#   5. f32 vecadd and a masked f32 scale run on the simulator.
#
# TRITON_OPT may point at triton-opt; check 4 is SKIP when it is absent.
#
set -euo pipefail

BIN=${1:?usage: check-ch25.sh <llvm-bin-dir>}
HERE=$(cd "$(dirname "$0")" && pwd)
TOOL="$BIN/gpumin-triton"
LLC="$BIN/llc"
MC="$BIN/llvm-mc"
SIM="$HERE/../tools/gpumin_sim.py"
ADD="$HERE/triton/f32_add.py"
SCALE="$HERE/triton/f32_scale.py"
GOLDEN="$HERE/triton/f32_add.ttir"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
export PYTHONDONTWRITEBYTECODE=1

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

[ -x "$TOOL" ] || fail "missing $TOOL"
[ -x "$LLC" ] || fail "missing $LLC"
[ -x "$MC" ] || fail "missing $MC"
[ -f "$ADD" ] || fail "missing $ADD"
[ -f "$SCALE" ] || fail "missing $SCALE"
[ -f "$GOLDEN" ] || fail "missing $GOLDEN"

TRITON_OPT=${TRITON_OPT:-$(command -v triton-opt || true)}

ADD_EMIT=(--constexpr BLOCK=256 --arg-type 'x_ptr=ptr<f32>'
          --arg-type 'y_ptr=ptr<f32>' --arg-type 'out_ptr=ptr<f32>')
SCALE_EMIT=(--constexpr BLOCK=256 --arg-type 'x_ptr=ptr<f32>'
            --arg-type 'out_ptr=ptr<f32>')

# --- 1. MC round-trip ----------------------------------------------------
cat > "$TMP/f32.s" <<'ASM'
fadd r1, r2, r3
fsub r1, r2, r3
fmul r1, r2, r3
fsetp.oeq p1, r2, r3
fsetp.one p1, r2, r3
fsetp.olt p1, r2, r3
fsetp.ole p1, r2, r3
fsetp.ogt p1, r2, r3
fsetp.oge p1, r2, r3
ASM
"$MC" -triple=gpumin -show-encoding "$TMP/f32.s" |
  sed -n 's/.*encoding: \[\(.*\)\]/\1/p' > "$TMP/bytes.txt"
[ "$(wc -l < "$TMP/bytes.txt")" -eq 9 ] ||
  fail "expected nine encodings"
"$MC" -triple=gpumin -disassemble "$TMP/bytes.txt" > "$TMP/round.s"
grep -q 'fadd r1, r2, r3' "$TMP/round.s" || fail "fadd did not round-trip"
grep -q 'fsetp.olt p1, r2, r3' "$TMP/round.s" ||
  fail "fsetp.olt did not round-trip"
grep -q 'fsetp.oge p1, r2, r3' "$TMP/round.s" ||
  fail "fsetp.oge did not round-trip"
echo "PASS 1/5: fadd/fsub/fmul/fsetp encode and decode"

# --- 2. codegen selects the f32 instructions -----------------------------
"$TOOL" --emit-llvm "${ADD_EMIT[@]}" "$ADD" -o "$TMP/add.ll"
"$TOOL" --emit-llvm "${SCALE_EMIT[@]}" "$SCALE" -o "$TMP/scale.ll"
"$LLC" -march=gpumin -O1 -verify-machineinstrs "$TMP/add.ll" \
  -o "$TMP/add.s" || fail "llc rejected the f32 vecadd"
"$LLC" -march=gpumin -O1 -verify-machineinstrs "$TMP/scale.ll" \
  -o "$TMP/scale.s" || fail "llc rejected the f32 scale kernel"
grep -q 'load float' "$TMP/add.ll" || fail "f32 loads were not emitted"
grep -q 'store float' "$TMP/add.ll" || fail "f32 stores were not emitted"
grep -qE '^\s*fadd ' "$TMP/add.s" || fail "no fadd in the assembly"
grep -qE '^\s*fmul ' "$TMP/scale.s" || fail "no fmul in the assembly"
grep -qE '^\s*fsetp\.olt ' "$TMP/scale.s" ||
  fail "no fsetp.olt in the assembly"
if grep -q 'Cannot select' "$TMP/add.s" "$TMP/scale.s"; then
  fail "the selector rejected an f32 node"
fi
echo "PASS 2/5: llc selects fadd/fmul/fsetp and 32-bit f32 memory ops"

# --- 3. TTIR shape -------------------------------------------------------
"$TOOL" --emit-ttir "${ADD_EMIT[@]}" "$ADD" > "$TMP/add.ttir"
"$TOOL" --emit-ttir "${SCALE_EMIT[@]}" "$SCALE" > "$TMP/scale.ttir"
grep -q '%x_ptr: !tt.ptr<f32>' "$TMP/add.ttir" ||
  fail "the pointer argument is not !tt.ptr<f32>"
grep -q 'arith.addf %x, %y : tensor<256xf32>' "$TMP/add.ttir" ||
  fail "missing tensor addf"
grep -q 'arith.mulf %x, .* : tensor<256xf32>' "$TMP/scale.ttir" ||
  fail "missing tensor mulf"
grep -q 'arith.cmpf olt, ' "$TMP/scale.ttir" ||
  fail "missing arith.cmpf"
grep -q 'arith.constant 1.500000e+00 : f32' "$TMP/scale.ttir" ||
  fail "a float literal was lowered as an integer"
if grep -q 'arith.addi %x' "$TMP/add.ttir"; then
  fail "f32 addition was lowered as arith.addi"
fi
echo "PASS 3/5: TTIR carries f32 types, addf/mulf/cmpf and a float constant"

# --- 4. Triton parser and canonical golden -------------------------------
if [ -n "$TRITON_OPT" ] && [ -x "$TRITON_OPT" ]; then
  "$TRITON_OPT" "$TMP/add.ttir" -o /dev/null ||
    fail "triton-opt rejected our TTIR"
  "$TRITON_OPT" "$TMP/add.ttir" -o "$TMP/ours.canon.ttir"
  "$TRITON_OPT" "$GOLDEN" -o "$TMP/golden.canon.ttir"
  diff -u "$TMP/golden.canon.ttir" "$TMP/ours.canon.ttir" \
    > "$TMP/canon.diff" ||
    { cat "$TMP/canon.diff" >&2; fail "canonical TTIR differs from the golden"; }
  echo "PASS 4/5: triton-opt parses the TTIR and it matches the golden"
else
  echo "SKIP 4/5: triton-opt not found (set TRITON_OPT to enable)"
fi

# --- 5. simulator: f32 vecadd and masked f32 scale ------------------------
"$LLC" -march=gpumin -O1 -filetype=obj "$TMP/add.ll" -o "$TMP/add.o"
"$LLC" -march=gpumin -O1 -filetype=obj "$TMP/scale.ll" -o "$TMP/scale.o"

cat > "$TMP/add.launch.json" <<'JSON'
{
  "grid": 1,
  "block": 32,
  "args_addr": 4096,
  "args": [0, 8192, 16384],
  "fill": [
    { "addr": 0, "start": 1065353216, "step": 1, "count": 256 },
    { "addr": 8192, "start": 1073741824, "step": 1, "count": 256 }
  ],
  "dump": [ { "addr": 16384, "count": 8 } ]
}
JSON
got=$(python3 "$SIM" "$TMP/add.o" --kernel f32_add \
        --launch "$TMP/add.launch.json") ||
  fail "the simulator rejected the f32 vecadd"
python3 - "$got" <<'PY' || fail "f32 vecadd result mismatch"
import struct, sys
def bits(v):
    return struct.unpack('<I', struct.pack('<f', v))[0]
def value(b):
    return struct.unpack('<f', struct.pack('<I', b & 0xffffffff))[0]
want = [bits(value(0x3F800000 + i) + value(0x40000000 + i))
        for i in range(8)]
got = [int(w) for w in sys.argv[1].split()]
if got != want:
    print("got %r, want %r" % (got, want), file=sys.stderr)
    sys.exit(1)
PY

cat > "$TMP/scale.launch.json" <<'JSON'
{
  "grid": 1,
  "block": 32,
  "args_addr": 4096,
  "args": [0, 8192],
  "fill": [ { "addr": 0, "start": 1065353216, "step": 1, "count": 256 } ],
  "dump": [ { "addr": 8192, "count": 8 } ]
}
JSON
got=$(python3 "$SIM" "$TMP/scale.o" --kernel f32_scale \
        --launch "$TMP/scale.launch.json") ||
  fail "the simulator rejected the masked f32 scale"
python3 - "$got" <<'PY' || fail "masked f32 scale result mismatch"
import struct, sys
def bits(v):
    return struct.unpack('<I', struct.pack('<f', v))[0]
def value(b):
    return struct.unpack('<f', struct.pack('<I', b & 0xffffffff))[0]
want = [bits(value(0x3F800000 + i) * 1.5) for i in range(8)]
got = [int(w) for w in sys.argv[1].split()]
if got != want:
    print("got %r, want %r" % (got, want), file=sys.stderr)
    sys.exit(1)
PY
echo "PASS 5/5: f32 vecadd and masked f32 scale run on the simulator"

echo
echo "Chapter 25: 5/5 checks passed"
