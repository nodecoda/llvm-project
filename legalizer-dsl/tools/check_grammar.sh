#!/bin/sh
# Grammar gate: compile with -Wall (0/0/0), accept the v7 corpus, accept the
# v6 corpus unchanged (set V6_CORPUS), and reject every negative case.
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
build=${TMPDIR:-/tmp}/dslbuild
mkdir -p "$build"

bison -Wall -d -o "$build/v7.cc" "$here/legalize-decl.v7.y" 2>"$build/bison.err"
if [ -s "$build/bison.err" ]; then cat "$build/bison.err"; exit 1; fi

g++ -std=c++17 -I"$build" -o "$build/parse_check" \
    "$here/tools/parse_check.cc" "$build/v7.cc"

"$build/parse_check" "$here/tests/v7-shapes.decl"
"$build/parse_check" "$here/tests/v8-env.decl"
[ -n "$V6_CORPUS" ] && "$build/parse_check" "$V6_CORPUS"

# Every negative file must fail; otherwise "the parser accepts everything"
# would pass this gate.
rc=0
for f in "$here"/tests/*negative*.decl; do
  if "$build/parse_check" "$f" >/dev/null 2>&1; then
    echo "FAIL negative case parsed: $f" >&2
    rc=1
  else
    echo "negative case rejected (expected): $(basename "$f")"
  fi
done
exit $rc
