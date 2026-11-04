#!/usr/bin/env bash
# Reproduce the two TableGen crashes and their controls.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
TBLGEN=${TBLGEN:-/home/ubuntu/llvm/builds/pr-lane/bin/llvm-tblgen}
for f in "$HERE"/*.td; do
  "$TBLGEN" -print-records "$f" >/dev/null 2>"$HERE/.err.tmp"
  rc=$?
  printf '%-48s rc=%s  %s\n' "$(basename "$f")" "$rc" \
    "$(grep -m1 -E 'Assertion|Segmentation|note:' "$HERE/.err.tmp" | cut -c1-90)"
done
rm -f "$HERE/.err.tmp"
