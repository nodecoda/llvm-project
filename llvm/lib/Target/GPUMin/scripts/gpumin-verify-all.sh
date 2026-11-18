#!/usr/bin/env bash
#
# GPUMin 8-gate acceptance harness.
#
# Usage: gpumin-verify-all.sh <llvm-bin-dir>
#
# Gates 1-5 and 8 are the static surface fixed by chapter 10.  Gates 6 and 7
# are wired in here from the start but report SKIP until the simulator and
# launch code (chapters 12-13) and the scheduling model (chapter 18) exist;
# the MVP stop condition is 8/8, not "the static part still builds".
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

# --- 6. simulator and launch e2e -----------------------------------------
if [ -x "$HERE/check-ch12.sh" ] || [ -x "$HERE/check-ch13.sh" ]; then
  ok6=1
  for gate in check-ch12.sh check-ch13.sh check-ch14.sh check-ch15.sh check-ch16.sh check-ch17.sh check-ch18.sh check-ch19.sh check-ch20.sh; do
    if [ -x "$HERE/$gate" ]; then
      if ! "$HERE/$gate" "$BIN" > "$TMP/$gate.log" 2>&1; then
        cat "$TMP/$gate.log" >&2
        ok6=0
      fi
    fi
  done
  if [ "$ok6" = 1 ]; then
    pass 6 "simulator and launch e2e"
  else
    fail 6 "simulator and launch e2e"
  fi
else
  skip 6 "simulator and launch e2e arrive with chapters 12-13"
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
if [ -x "$HERE/check-ch11.sh" ]; then
  if ! "$HERE/check-ch11.sh" "$BIN" > "$TMP/ch11.log" 2>&1; then
    cat "$TMP/ch11.log" >&2
    ok=0
  fi
fi
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
[ -f "$HERE/../docs/11-"*.md ] || { echo "missing doc 11" >&2; ok=0; }
[ -x "$HERE/check-ch11.sh" ] || { echo "missing gate 11" >&2; ok=0; }
[ -f "$HERE/../docs/31-"*.md ] || { echo "missing doc 31" >&2; ok=0; }
[ -x "$HERE/check-ch31.sh" ] || { echo "missing gate 31" >&2; ok=0; }
[ -f "$HERE/../docs/32-"*.md ] || { echo "missing doc 32" >&2; ok=0; }
[ -x "$HERE/check-ch32.sh" ] || { echo "missing gate 32" >&2; ok=0; }
[ -f "$HERE/../docs/33-"*.md ] || { echo "missing doc 33" >&2; ok=0; }
[ -x "$HERE/check-ch33.sh" ] || { echo "missing gate 33" >&2; ok=0; }
[ -f "$HERE/../docs/34-"*.md ] || { echo "missing doc 34" >&2; ok=0; }
[ -x "$HERE/check-ch34.sh" ] || { echo "missing gate 34" >&2; ok=0; }
[ -f "$HERE/../docs/35-"*.md ] || { echo "missing doc 35" >&2; ok=0; }
[ -x "$HERE/check-ch35.sh" ] || { echo "missing gate 35" >&2; ok=0; }
[ -f "$HERE/../docs/12-"*.md ] || { echo "missing doc 12" >&2; ok=0; }
[ -x "$HERE/check-ch12.sh" ] || { echo "missing gate 12" >&2; ok=0; }
[ -f "$HERE/../docs/13-"*.md ] || { echo "missing doc 13" >&2; ok=0; }
[ -x "$HERE/check-ch13.sh" ] || { echo "missing gate 13" >&2; ok=0; }
[ -f "$HERE/../docs/14-"*.md ] || { echo "missing doc 14" >&2; ok=0; }
[ -x "$HERE/check-ch14.sh" ] || { echo "missing gate 14" >&2; ok=0; }
[ -f "$HERE/../docs/15-"*.md ] || { echo "missing doc 15" >&2; ok=0; }
[ -x "$HERE/check-ch15.sh" ] || { echo "missing gate 15" >&2; ok=0; }
[ -f "$HERE/../docs/16-"*.md ] || { echo "missing doc 16" >&2; ok=0; }
[ -x "$HERE/check-ch16.sh" ] || { echo "missing gate 16" >&2; ok=0; }
[ -f "$HERE/../docs/17-"*.md ] || { echo "missing doc 17" >&2; ok=0; }
[ -x "$HERE/check-ch17.sh" ] || { echo "missing gate 17" >&2; ok=0; }
[ -f "$HERE/../docs/18-"*.md ] || { echo "missing doc 18" >&2; ok=0; }
[ -x "$HERE/check-ch18.sh" ] || { echo "missing gate 18" >&2; ok=0; }
[ -f "$HERE/../docs/19-"*.md ] || { echo "missing doc 19" >&2; ok=0; }
[ -x "$HERE/check-ch19.sh" ] || { echo "missing gate 19" >&2; ok=0; }
[ -f "$HERE/../docs/20-"*.md ] || { echo "missing doc 20" >&2; ok=0; }
[ -x "$HERE/check-ch20.sh" ] || { echo "missing gate 20" >&2; ok=0; }
[ -f "$HERE/../docs/21-"*.md ] || { echo "missing doc 21" >&2; ok=0; }
[ -x "$HERE/check-ch21.sh" ] || { echo "missing gate 21" >&2; ok=0; }
[ -f "$HERE/../docs/22-"*.md ] || { echo "missing doc 22" >&2; ok=0; }
[ -x "$HERE/check-ch22.sh" ] || { echo "missing gate 22" >&2; ok=0; }
[ -f "$HERE/../docs/23-"*.md ] || { echo "missing doc 23" >&2; ok=0; }
[ -x "$HERE/check-ch23.sh" ] || { echo "missing gate 23" >&2; ok=0; }
[ -f "$HERE/../docs/24-"*.md ] || { echo "missing doc 24" >&2; ok=0; }
[ -x "$HERE/check-ch24.sh" ] || { echo "missing gate 24" >&2; ok=0; }
[ -f "$HERE/../docs/25-"*.md ] || { echo "missing doc 25" >&2; ok=0; }
[ -x "$HERE/check-ch25.sh" ] || { echo "missing gate 25" >&2; ok=0; }
[ -f "$HERE/../docs/26-"*.md ] || { echo "missing doc 26" >&2; ok=0; }
[ -x "$HERE/check-ch26.sh" ] || { echo "missing gate 26" >&2; ok=0; }
[ -f "$HERE/../docs/27-"*.md ] || { echo "missing doc 27" >&2; ok=0; }
[ -x "$HERE/check-ch27.sh" ] || { echo "missing gate 27" >&2; ok=0; }
[ -f "$HERE/../docs/28-"*.md ] || { echo "missing doc 28" >&2; ok=0; }
[ -x "$HERE/check-ch28.sh" ] || { echo "missing gate 28" >&2; ok=0; }
[ -f "$HERE/../docs/29-"*.md ] || { echo "missing doc 29" >&2; ok=0; }
[ -x "$HERE/check-ch29.sh" ] || { echo "missing gate 29" >&2; ok=0; }
[ -f "$HERE/../docs/30-"*.md ] || { echo "missing doc 30" >&2; ok=0; }
[ -x "$HERE/check-ch30.sh" ] || { echo "missing gate 30" >&2; ok=0; }

missing=$(grep -L '退出条件' "$HERE/../docs"/[0-9][0-9]-*.md || true)
if [ -n "$missing" ]; then
  echo "chapter docs without an exit condition: $missing" >&2
  ok=0
fi

if [ "$ok" = 1 ]; then
  pass 8 "docs, SIMT model and static counts agree"
else
  fail 8 "docs/DSL/counts"
fi

echo
echo "Gates: $PASSED passed, $FAILED failed, $SKIPPED skipped"
[ "$FAILED" -eq 0 ]
