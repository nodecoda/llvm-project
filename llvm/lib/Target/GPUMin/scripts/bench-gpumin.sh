#!/usr/bin/env bash
#
# Chapter 37 benchmark harness: compile, run and tabulate the course-4 kernels.
#
# Usage: bench-gpumin.sh <llvm-bin-dir> [--write-baseline <file>]
#
# Every program is an existing e2e directory; expected.json supplies the kernel
# name, the argument types and any constexpr values, so the harness never
# duplicates launch metadata.  The simulator's --stats line (stderr) is reduced
# to five numbers: issued, cycles, glb_ld, shm_ld and loc_st (local stores, the
# spill proxy).
#
# The default output is a markdown table; --write-baseline rewrites the
# machine-readable file that check-ch37.sh compares against.  The simulator is
# deterministic, so the gate asserts "current <= baseline" rather than
# equality, which leaves room for later optimizations.
#
set -euo pipefail

BIN=${1:?usage: bench-gpumin.sh <llvm-bin-dir> [--write-baseline <file>]}
shift
WRITE_BASELINE=
if [ "${1:-}" = "--write-baseline" ]; then
  WRITE_BASELINE=${2:?--write-baseline needs a path}
  shift 2
fi
[ $# -eq 0 ] || { echo "usage: bench-gpumin.sh <llvm-bin-dir> [--write-baseline <file>]" >&2; exit 1; }

HERE=$(cd "$(dirname "$0")" && pwd)
TOOL="$BIN/gpumin-triton"
LLC="$BIN/llc"
SIM="$HERE/../tools/gpumin_sim.py"
E2E="$HERE/triton/e2e"
TMP=$(mktemp -d)
trap 'python3 -c "import shutil,sys; shutil.rmtree(sys.argv[1], ignore_errors=True)" "$TMP"' EXIT
export PYTHONDONTWRITEBYTECODE=1

[ -x "$TOOL" ] || { echo "missing $TOOL" >&2; exit 1; }
[ -x "$LLC" ] || { echo "missing $LLC" >&2; exit 1; }

# label:program, in course order.
PROGRAMS="vecadd:add_f32 reduce:reduce matmul-rank1:matmul_no_dot \
matmul-dot:dot2d softmax:softmax2d attention:attention causal:causal"

metric() {
  # --stats prints one space-separated line on stderr.
  tr ' ' '\n' < "$2" | sed -n "s/^$1=//p"
}

bench_rows() {
  for entry in $PROGRAMS; do
    label=${entry%%:*}
    prog=${entry#*:}
    dir="$E2E/$prog"
    [ -f "$dir/kernel.py" ] || { echo "missing $dir/kernel.py" >&2; exit 1; }
    [ -f "$dir/expected.json" ] || { echo "missing $dir/expected.json" >&2; exit 1; }

    meta=$(python3 - "$dir/expected.json" <<'PY'
import json
import sys

doc = json.load(open(sys.argv[1]))
parts = [doc["kernel"]]
for name, ty in (doc.get("arg_types") or {}).items():
    parts += ["--arg-type", f"{name}={ty}"]
for name, value in (doc.get("constexpr") or {}).items():
    parts += ["--constexpr", f"{name}={value}"]
print(" ".join(parts))
PY
)
    read -r -a meta_arr <<< "$meta"
    kernel=${meta_arr[0]}
    emit=("${meta_arr[@]:1}")

    "$TOOL" --emit-llvm "${emit[@]}" "$dir/kernel.py" -o "$TMP/$prog.ll"
    "$LLC" -march=gpumin -O1 -filetype=obj "$TMP/$prog.ll" -o "$TMP/$prog.o"
    python3 "$SIM" "$TMP/$prog.o" --kernel "$kernel" \
      --launch "$dir/launch.json" --stats > /dev/null 2> "$TMP/$prog.stats"

    issued=$(metric issued "$TMP/$prog.stats")
    cycles=$(metric cycles "$TMP/$prog.stats")
    glb_ld=$(metric glb_ld "$TMP/$prog.stats")
    shm_ld=$(metric shm_ld "$TMP/$prog.stats")
    loc_st=$(metric loc_st "$TMP/$prog.stats")
    for value in "$issued" "$cycles" "$glb_ld" "$shm_ld" "$loc_st"; do
      [ -n "$value" ] || { echo "$prog: missing a stat in --stats" >&2; exit 1; }
    done

    if [ -n "$WRITE_BASELINE" ]; then
      printf '%s issued=%s cycles=%s glb_ld=%s shm_ld=%s loc_st=%s\n' \
        "$label" "$issued" "$cycles" "$glb_ld" "$shm_ld" "$loc_st"
    else
      printf '| `%s` | %s | %s | %s | %s | %s |\n' \
        "$label" "$issued" "$cycles" "$glb_ld" "$shm_ld" "$loc_st"
    fi
  done
}

if [ -n "$WRITE_BASELINE" ]; then
  bench_rows > "$WRITE_BASELINE"
  echo "wrote $WRITE_BASELINE"
else
  printf '| kernel | issued | cycles | glb_ld | shm_ld | loc_st |\n'
  printf '| --- | --- | --- | --- | --- | --- |\n'
  bench_rows
fi
