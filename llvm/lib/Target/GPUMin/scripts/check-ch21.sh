#!/usr/bin/env bash
#
# Chapter 21 gate: the C++ Triton frontend and its CPython AST boundary.
#
# Usage: check-ch21.sh <llvm-bin-dir>
#
# What this proves:
#   1. gpumin-triton runs CPython's ast.parse and recognises @triton.jit;
#   2. a tl.constexpr parameter is distinguished from a runtime parameter;
#   3. the AST can be dumped to JSON and re-read without Python (--ast-json);
#   4. a file without a kernel is rejected with a useful message;
#   5. --emit-ttir has the chapter-21 module shell in place.
#
set -euo pipefail

BIN=${1:?usage: check-ch21.sh <llvm-bin-dir>}
HERE=$(cd "$(dirname "$0")" && pwd)
TOOL="$BIN/gpumin-triton"
KERNEL="$HERE/triton/add_i32.py"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
export PYTHONDONTWRITEBYTECODE=1

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

[ -x "$TOOL" ] || fail "missing $TOOL"
[ -f "$KERNEL" ] || fail "missing $KERNEL"

# --- 1. kernel and argument discovery ------------------------------------
"$TOOL" --dump-ast "$KERNEL" > "$TMP/ast.txt"
grep -q '^kernel add_kernel' "$TMP/ast.txt" ||
  fail "did not find add_kernel"
grep -q '^  arg x_ptr$' "$TMP/ast.txt" ||
  fail "did not find x_ptr"
grep -q '^  arg BLOCK (constexpr)$' "$TMP/ast.txt" ||
  fail "did not mark BLOCK as constexpr"
grep -q '^  arg n$' "$TMP/ast.txt" ||
  fail "did not find runtime argument n"
echo "PASS 1/5: @triton.jit and tl.constexpr are recognised"

# --- 2. JSON is a real CPython AST ---------------------------------------
"$TOOL" --dump-ast-json "$KERNEL" -o "$TMP/ast.json"
grep -q '"_type":"FunctionDef"' "$TMP/ast.json" ||
  fail "AST JSON has no FunctionDef"
grep -q '"name":"add_kernel"' "$TMP/ast.json" ||
  fail "AST JSON has no add_kernel name"
grep -q '"_type":"Attribute"' "$TMP/ast.json" ||
  fail "AST JSON has no decorator/annotation nodes"
echo "PASS 2/5: --dump-ast-json emits the CPython AST"

# --- 3. the C++ side can work without Python ----------------------------
"$TOOL" --ast-json "$TMP/ast.json" --dump-ast > "$TMP/from-json.txt"
diff -u "$TMP/ast.txt" "$TMP/from-json.txt" > /dev/null ||
  fail "--ast-json produced a different signature summary"
echo "PASS 3/5: --ast-json re-reads the dumped tree without Python"

# --- 4. no kernel is an error -------------------------------------------
cat > "$TMP/plain.py" <<'PY'
def not_a_kernel(x):
    return x
PY
if "$TOOL" --dump-ast "$TMP/plain.py" > "$TMP/plain.out" 2> "$TMP/plain.err"; then
  fail "a file without @triton.jit was accepted"
fi
grep -q 'no @triton.jit kernel found' "$TMP/plain.err" ||
  fail "missing-kernel error message is not helpful"
echo "PASS 4/5: a file without a kernel is rejected"

# --- 5. the TTIR shell is present ---------------------------------------
# Chapter 22 replaces the empty shell with real lowering, so feed a scalar
# kernel here; the check is that --emit-ttir always prints a module.
cat > "$TMP/scalar.py" <<'PY'
import triton
import triton.language as tl

@triton.jit
def scalar_add(n):
    return n + 1
PY
"$TOOL" --emit-ttir "$TMP/scalar.py" > "$TMP/kernel.ttir"
grep -q '^module {$' "$TMP/kernel.ttir" ||
  fail "--emit-ttir did not print a module"
echo "PASS 5/5: --emit-ttir prints a TTIR module"
