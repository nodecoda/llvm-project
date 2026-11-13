#!/usr/bin/env bash
# MiniRV smoke: auto-td records, MC/CodeGen lit, differential e2e.
#
#   LLVM_BUILD_DIR=<build> llvm/lib/Target/MiniRV/dev/minirv-smoke.sh
set -euo pipefail
TREE=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
BUILD=${LLVM_BUILD_DIR:-/home/ubuntu/llvm/builds/dsl-tablegen}
B="$BUILD/bin"
cd "$TREE"

echo "== auto-td unit tests =="
python3 -m unittest discover -s llvm/utils/auto-td/tests -p 'test_*.py'

echo "== generated records =="
python3 llvm/utils/auto-td/auto-td-gen --target-root llvm/lib/Target/MiniRV \
  --riscv-opcodes llvm/lib/Target/MiniRV/auto-td/opcodes \
  --out-dir /tmp/minirv-smoke --check-only

echo "== MC/CodeGen lit =="
"$B/llvm-lit" --no-progress-bar llvm/test/CodeGen/MiniRV llvm/test/MC/MiniRV

echo "== end-to-end =="
"$TREE/llvm/lib/Target/MiniRV/dev/e2e/run.sh"

echo "== static code size =="
"$TREE/llvm/lib/Target/MiniRV/dev/bench/run.sh"
