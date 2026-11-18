#!/usr/bin/env bash
#
# GPUMin 8-gate acceptance harness.
#
# Usage: gpumin-verify-all.sh <llvm-bin-dir>
#
# Gates 1-5 and 8 are the static surface fixed by chapter 10.  Gates 6 and 7
# are wired in here from the start but report SKIP until the simulator
# (chapter 12) and the scheduling model (chapter 18) exist; the MVP stop
# condition is 8/8, not "the static part still builds".
#
set -uo pipefail

BIN=${1:?usage: gpumin-verify-all.sh <llvm-bin-dir>}
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../../../.." && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

FAILED=0
PASSED=0
SKIPPED=0

pass() {
  PASSED=$((PASSED + 1))
  echo "PASS $1/8: $2"
}

skip() {
  SKIPPED=$((SKIPPED + 1))
  echo "SKIP $1/8: $2"
}

fail() {
  FAILED=$((FAILED + 1))
  echo "FAIL $1/8: $2" >&2
}

# --- 1. target registry ---------------------------------------------------
if "$BIN/llc" --version | grep -qw gpumin &&
   printf 'exit\n' | "$BIN/llvm-mc" -triple=gpumin -filetype=obj \
                    -o /dev/null 2>/dev/null; then
  pass 1 "target registry: llc lists gpumin and llvm-mc assembles it"
else
  fail 1 "target registry"
fi

# --- 2. MC encoding vs the independent encoder ---------------------------
if "$HERE/check-ch02.sh" "$BIN" > "$TMP/ch02.log" 2>&1; then
  pass 2 "MC encoding matches tools/gpumin-encode.py byte for byte"
else
  cat "$TMP/ch02.log" >&2
  fail 2 "MC encoding vs independent encoder"
fi

# --- 3. disassembly round-trip -------------------------------------------
if "$BIN/llvm-lit" -q "$SRC/test/MC/GPUMin" > "$TMP/mc-lit.log" 2>&1; then
  pass 3 "MC lit: assemble, disassemble and re-encode"
else
  cat "$TMP/mc-lit.log" >&2
  fail 3 "MC disassembly round-trip"
fi

# --- 4. CodeGen lit -------------------------------------------------------
if "$BIN/llvm-lit" -q "$SRC/test/CodeGen/GPUMin" > "$TMP/cg-lit.log" 2>&1; then
  pass 4 "CodeGen lit: basic/alu/control/memory/calls/types/intrinsics"
else
  cat "$TMP/cg-lit.log" >&2
  fail 4 "CodeGen lit"
fi

# --- 5. static e2e: IR -> .s -> .o ---------------------------------------
VECADD="$HERE/e2e/vecadd.ll"
if "$BIN/llc" -march=gpumin -O1 -verify-machineinstrs "$VECADD" \
      -o "$TMP/vecadd.s" &&
   "$BIN/llc" -march=gpumin -O1 -filetype=obj "$VECADD" \
      -o "$TMP/vecadd.o" &&
   "$BIN/llvm-mc" -triple=gpumin -filetype=obj "$TMP/vecadd.s" \
      -o "$TMP/vecadd-mc.o"; then
  ok=1
  grep -q 'sr.tid' "$TMP/vecadd.s" || ok=0
  [ "$(grep -c 'ld.global.b32' "$TMP/vecadd.s")" -eq 2 ] || ok=0
  grep -q 'st.global.b32' "$TMP/vecadd.s" || ok=0
  grep -q $'^\texit$' "$TMP/vecadd.s" || ok=0
  "$BIN/llvm-objdump" -d -r "$TMP/vecadd.o" | grep -v 'file format' > "$TMP/a.txt"
  "$BIN/llvm-objdump" -d -r "$TMP/vecadd-mc.o" | grep -v 'file format' > "$TMP/b.txt"
  diff -u "$TMP/a.txt" "$TMP/b.txt" > "$TMP/e2e.diff" || ok=0
  if [ "$ok" = 1 ]; then
    pass 5 "static e2e: vecadd IR -> object, and .o == reassembled .s"
  else
    cat "$TMP/e2e.diff" >&2 2>/dev/null || true
    fail 5 "static e2e"
  fi
else
  fail 5 "static e2e (llc/llvm-mc failed)"
fi

# --- 6. simulator e2e -----------------------------------------------------
if [ -x "$HERE/check-ch12.sh" ]; then
  if "$HERE/check-ch12.sh" "$BIN" > "$TMP/ch12.log" 2>&1; then
    pass 6 "simulator e2e"
  else
    cat "$TMP/ch12.log" >&2
    fail 6 "simulator e2e"
  fi
else
  skip 6 "simulator e2e arrives with chapter 12"
fi

# --- 7. scheduling / occupancy -------------------------------------------
if [ -x "$HERE/check-ch18.sh" ]; then
  if "$HERE/check-ch18.sh" "$BIN" > "$TMP/ch18.log" 2>&1; then
    pass 7 "scheduling and occupancy"
  else
    cat "$TMP/ch18.log" >&2
    fail 7 "scheduling and occupancy"
  fi
else
  skip 7 "scheduling model arrives with chapter 18"
fi

# --- 8. docs / DSL and the locked static counts --------------------------
ok=1
# shellcheck source=/dev/null
. "$HERE/static-counts.txt"

# Keep the locked values before the actual counts reuse the same names.
want_mc_lit_files=$mc_lit_files
want_codegen_lit_files=$codegen_lit_files
want_chapter_docs=$chapter_docs
want_chapter_gates=$chapter_gates
want_e2e_programs=$e2e_programs
want_mc_instructions=$mc_instructions

mc_lit_files=$(find "$SRC/test/MC/GPUMin" -maxdepth 1 -name '*.s' | wc -l)
codegen_lit_files=$(find "$SRC/test/CodeGen/GPUMin" -maxdepth 1 -name '*.ll' | wc -l)
chapter_docs=$(find "$HERE/../docs" -maxdepth 1 -name '[0-9][0-9]-*.md' | wc -l)
chapter_gates=$(find "$HERE" -maxdepth 1 -name 'check-ch[0-9][0-9].sh' | wc -l)
e2e_programs=$(find "$HERE/e2e" -maxdepth 1 -name '*.ll' | wc -l)
mc_instructions=$(cat "$SRC/test/MC/GPUMin"/*.s |
                  grep -cE '^[[:space:]]*[a-z][a-z0-9.]*')

check_count() {
  local name=$1 actual=$2 want=$3
  if [ "$actual" -ne "$want" ]; then
    echo "count $name: got $actual, want $want" >&2
    ok=0
  fi
}

check_count mc_lit_files "$mc_lit_files" "$want_mc_lit_files"
check_count codegen_lit_files "$codegen_lit_files" "$want_codegen_lit_files"
check_count chapter_docs "$chapter_docs" "$want_chapter_docs"
check_count chapter_gates "$chapter_gates" "$want_chapter_gates"
check_count e2e_programs "$e2e_programs" "$want_e2e_programs"
check_count mc_instructions "$mc_instructions" "$want_mc_instructions"

for n in 00 01 02 03 04 05 06 07 08 09; do
  [ -f "$HERE/../docs/$n-"*.md ] || { echo "missing doc $n" >&2; ok=0; }
  [ -x "$HERE/check-ch$n.sh" ] || { echo "missing gate $n" >&2; ok=0; }
done
[ -f "$HERE/../docs/10-"*.md ] || { echo "missing doc 10" >&2; ok=0; }
[ -x "$HERE/gpumin-verify-all.sh" ] ||
  { echo "missing the chapter-10 harness" >&2; ok=0; }

missing=$(grep -L '退出条件' "$HERE/../docs"/[0-9][0-9]-*.md || true)
if [ -n "$missing" ]; then
  echo "chapter docs without an exit condition: $missing" >&2
  ok=0
fi

if [ "$ok" = 1 ]; then
  pass 8 "docs/gates present and static counts locked"
else
  fail 8 "docs/DSL/counts"
fi

echo
echo "Gates: $PASSED passed, $FAILED failed, $SKIPPED skipped"
[ "$FAILED" -eq 0 ]
