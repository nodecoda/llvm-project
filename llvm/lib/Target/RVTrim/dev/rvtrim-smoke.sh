#!/usr/bin/env bash
# RVTrim smoke: profile gate, auto-td records, MC/CodeGen lit, autovec kernels.
#
#   LLVM_BUILD_DIR=<build> llvm/lib/Target/RVTrim/dev/rvtrim-smoke.sh
set -euo pipefail
TREE=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
BUILD=${LLVM_BUILD_DIR:-/home/ubuntu/llvm/builds/dsl-tablegen}
B="$BUILD/bin"
cd "$TREE"

echo "== auto-td unit tests =="
python3 -m unittest discover -s llvm/utils/auto-td/tests -p 'test_*.py'

echo "== generated records =="
python3 llvm/utils/auto-td/auto-td-gen --target-root llvm/lib/Target/RVTrim \
  --riscv-opcodes llvm/lib/Target/RVTrim/auto-td/opcodes \
  --out-dir /tmp/rvsmoke --check-only

echo "== V record properties (golden) =="
RECORDS=$(mktemp)
trap 'rm -f "$RECORDS"' EXIT
"$B/llvm-tblgen" -print-records \
  -I "$TREE/llvm/include" -I "$TREE/llvm/lib/Target/RVTrim" \
  -I "$BUILD/lib/Target/RVTrim" "$TREE/llvm/lib/Target/RVTrim/RVTrim.td" > "$RECORDS"
python3 llvm/utils/auto-td/tests/compare_record_props.py \
  --records "$RECORDS" \
  --golden llvm/lib/Target/RVTrim/auto-td/golden/v-all-props.txt
python3 llvm/utils/auto-td/tests/compare_riscv_records.py \
  --inc "$BUILD/lib/Target/RVTrim/auto-td/AutoTdInstrInfo.inc" \
  --records "$RECORDS"

echo "== MC/CodeGen lit =="
"$B/llvm-lit" --no-progress-bar llvm/test/MC/RVTrim llvm/test/CodeGen/RVTrim

echo "== whitespace =="
git diff --check

echo "== autovec kernels =="
"$TREE/llvm/lib/Target/RVTrim/dev/bench/run.sh"
