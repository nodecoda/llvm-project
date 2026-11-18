#!/usr/bin/env bash
#
# Chapter 37 gate: the benchmark harness and the course-4 wrap-up.
#
# Usage: check-ch37.sh <llvm-bin-dir>
#
# What this proves:
#   1. bench-gpumin.sh prints the seven-kernel markdown table in course order;
#   2. two runs produce byte-identical tables (the simulator is deterministic);
#   3. bench-baseline.txt covers exactly the benched kernels and all five
#      metrics;
#   4. no metric regressed past the committed upper bound, and the interesting
#      invariants still hold (dot uses shared, rank-1 does not, causal costs
#      more than attention);
#   5. the wrap-up document and README index the whole course.
#
set -euo pipefail

BIN=${1:?usage: check-ch37.sh <llvm-bin-dir>}
HERE=$(cd "$(dirname "$0")" && pwd)
BENCH="$HERE/bench-gpumin.sh"
BASE="$HERE/bench-baseline.txt"
DOC="$HERE/../docs/37-course4-wrap.md"
INDEX="$HERE/../docs/README.md"
TMP=$(mktemp -d)
trap 'python3 -c "import shutil,sys; shutil.rmtree(sys.argv[1], ignore_errors=True)" "$TMP"' EXIT
export PYTHONDONTWRITEBYTECODE=1

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

[ -x "$BENCH" ] || fail "missing $BENCH"
[ -f "$BASE" ] || fail "missing $BASE"
[ -f "$DOC" ] || fail "missing $DOC"

LABELS="vecadd reduce matmul-rank1 matmul-dot softmax attention causal"

# --- 1. the markdown table ----------------------------------------------
"$BENCH" "$BIN" > "$TMP/table.md"
grep -q '^| kernel | issued | cycles | glb_ld | shm_ld | loc_st |$' "$TMP/table.md" ||
  fail "the bench table header is missing"
[ "$(grep -c '^| `' "$TMP/table.md")" -eq 7 ] ||
  fail "expected seven kernel rows in the bench table"
for label in $LABELS; do
  grep -q "^| \`$label\` |" "$TMP/table.md" ||
    fail "the bench table is missing $label"
done
awk -F'|' '/^\| `/{ if (NF != 8) exit 1 }' "$TMP/table.md" ||
  fail "a bench row does not have six columns"
echo "PASS 1/5: the harness prints the seven-kernel markdown table"

# --- 2. determinism ------------------------------------------------------
"$BENCH" "$BIN" > "$TMP/table2.md"
cmp -s "$TMP/table.md" "$TMP/table2.md" ||
  fail "two bench runs produced different tables"
echo "PASS 2/5: two runs produce identical tables"

# --- 3. baseline coverage ------------------------------------------------
"$BENCH" "$BIN" --write-baseline "$TMP/current.txt" > /dev/null
if ! python3 - "$BASE" "$TMP/current.txt" <<'PY'
import sys


def parse(path):
    out = {}
    with open(path) as handle:
        for line in handle:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            out[parts[0]] = {k: int(v) for k, v in
                             (p.split("=", 1) for p in parts[1:])}
    return out


base = parse(sys.argv[1])
cur = parse(sys.argv[2])
want = ["issued", "cycles", "glb_ld", "shm_ld", "loc_st"]
if set(base) != set(cur):
    print("baseline kernels:", sorted(base), file=sys.stderr)
    print("current kernels:", sorted(cur), file=sys.stderr)
    sys.exit(1)
for label, metrics in base.items():
    for metric in want:
        if metric not in metrics or metric not in cur[label]:
            print(f"{label}: missing {metric}", file=sys.stderr)
            sys.exit(1)
PY
then
  fail "bench-baseline.txt does not match the benched kernels"
fi
echo "PASS 3/5: the baseline covers all seven kernels and five metrics"

# --- 4. upper bounds and the interesting invariants ----------------------
if ! python3 - "$BASE" "$TMP/current.txt" <<'PY'
import sys


def parse(path):
    out = {}
    with open(path) as handle:
        for line in handle:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            out[parts[0]] = {k: int(v) for k, v in
                             (p.split("=", 1) for p in parts[1:])}
    return out


base = parse(sys.argv[1])
cur = parse(sys.argv[2])
bad = 0
for label, metrics in base.items():
    for metric, bound in metrics.items():
        if cur[label][metric] > bound:
            print(f"REGRESSION {label}.{metric}: {cur[label][metric]} > {bound}",
                  file=sys.stderr)
            bad = 1
if bad:
    sys.exit(1)

# The wrapper-level invariants that make the table meaningful.
if cur["matmul-dot"]["shm_ld"] == 0 or cur["matmul-rank1"]["shm_ld"] != 0:
    print("dot no longer stages through shared memory", file=sys.stderr)
    sys.exit(1)
if cur["causal"]["issued"] <= cur["attention"]["issued"]:
    print("causal mask no longer costs anything over attention", file=sys.stderr)
    sys.exit(1)
if cur["reduce"]["shm_ld"] == 0:
    print("the reduction no longer round-trips through shared memory",
          file=sys.stderr)
    sys.exit(1)
PY
then
  fail "a benchmark metric regressed past the baseline"
fi
echo "PASS 4/5: every metric stays under the baseline and the invariants hold"

# --- 5. the wrap-up document and the index -------------------------------
grep -q '退出条件' "$DOC" || fail "the course-4 wrap-up has no 退出条件"
for marker in 'scf.if' 'shared' 'tt.dot' 'other=' 'bench-gpumin.sh'; do
  grep -q "$marker" "$DOC" || fail "the wrap-up does not mention $marker"
done
grep -q '| 37 |' "$INDEX" || fail "the docs index has no chapter 37 row"
grep -q '| `matmul-dot` |' "$DOC" ||
  fail "the wrap-up does not embed the benchmark table"
echo "PASS 5/5: the wrap-up document and index cover the course"

echo
echo "Chapter 37: 5/5 checks passed"
