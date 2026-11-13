#!/usr/bin/env bash
# Static code-size baseline for MiniRV against the RISC-V reference backend.
#
# The kernels are target-independent scalar loops, so the instruction counts
# show how much of the reference's code the trimmed backend reproduces.  They
# are not a performance claim.
set -euo pipefail
TREE=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
BUILD=${LLVM_BUILD_DIR:-/home/ubuntu/llvm/builds/dsl-tablegen}
B="$BUILD/bin"
cd "$(dirname "$0")"

printf '| kernel | MiniRV insns | riscv64 insns | ratio |\n'
printf '|---|---|---|---|\n'
for f in add mul reduction memcpy axpy; do
  m=$("$B/llc" -mtriple=minirv64 -O2 "$f.ll" -o - \
        | grep -cE $'^\t[a-z]' || true)
  r=$("$B/llc" -mtriple=riscv64 -mattr=+m -O2 "$f.ll" -o - \
        | grep -cE $'^\t[a-z]' || true)
  ratio=$(python3 -c "print(f'{$m/$r:.2f}')" 2>/dev/null || echo "-")
  printf '| %s | %s | %s | %s |\n' "$f" "$m" "$r" "$ratio"
done
