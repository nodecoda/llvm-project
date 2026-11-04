#!/usr/bin/env bash
# Reproduce every TableGen lab result into notes/tablegen/out/.
# Requires a built llvm-tblgen (assertions build preferred).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
OUT="$HERE/../out"
TBLGEN=${TBLGEN:-/home/ubuntu/llvm/builds/pr-lane/bin/llvm-tblgen}
mkdir -p "$OUT"

if [[ ! -x "$TBLGEN" ]]; then
  echo "llvm-tblgen not found: $TBLGEN" >&2
  echo "set TBLGEN=/path/to/llvm-tblgen" >&2
  exit 1
fi

echo "using: $TBLGEN"
"$TBLGEN" --version | head -3

for f in 01_core 02_types 03_ops 04_preproc 05_let_and_instances 06_defset_nested 07_list_unification; do
  echo "== $f"
  "$TBLGEN" -print-records "$HERE/$f.td" > "$OUT/$f.records.txt" 2> "$OUT/$f.err.txt"
  echo "rc=$?"
done

echo "== preprocessor variants"
{
  for d in "" "-DFEATURE_A" "-DFEATURE_A -DFEATURE_B" "-DFEATURE_B"; do
    echo "### [$d]"
    # shellcheck disable=SC2086
    "$TBLGEN" $d -print-records "$HERE/04_preproc.td" 2>&1 | grep '^def'
  done
} > "$OUT/04_preproc.variants.txt" 2>&1
"$TBLGEN" -E "$HERE/04_preproc.td" > "$OUT/04_preproc.E.txt" 2>&1

echo "== error matrix (no pipes, real exit codes)"
> "$OUT/errors.txt"
for f in "$HERE"/errors/*.td; do
  out=$("$TBLGEN" -print-records "$f" 2>&1); rc=$?
  { echo "### $(basename "$f") rc=$rc"; echo "$out" | head -6; } >> "$OUT/errors.txt"
done

echo "== known crash repro (expect SIGSEGV)"
"$TBLGEN" -print-records "$HERE/errors/25_multiclass_dump_crash.td" > "$OUT/crash-dump-multiclass.txt" 2>&1
echo "rc=$? (139 = SIGSEGV, expected)" >> "$OUT/crash-dump-multiclass.txt"

echo "== study backend (if built)"
if "$TBLGEN" -gen-study "$HERE/01_core.td" > "$OUT/01_core.study.txt" 2>/dev/null; then
  echo "wrote 01_core.study.txt"
else
  echo "(-gen-study unavailable: rebuild llvm-tblgen after adding StudyEmitter.cpp)"
fi

echo "done -> $OUT"
