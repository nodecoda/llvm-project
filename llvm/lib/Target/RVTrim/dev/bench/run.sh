#!/usr/bin/env bash
# Static RVV baseline for the RV64IMA_Zve64x profile.
#
# No RISC-V execution environment is available in this workspace, so this
# records vectorization shape and code size instead of wall-clock numbers.
set -euo pipefail
TREE=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
BUILD=${LLVM_BUILD_DIR:-/home/ubuntu/llvm/builds/dsl-tablegen}
B="$BUILD/bin"
cd "$(dirname "$0")"

printf '| kernel | profile | vectorized | vsetvli | v-mnemonics | insns |\n'
printf '|---|---|---|---|---|---|\n'
for f in add mul reduction memcpy axpy; do
  $B/opt -mtriple=rvtrim64 -mattr=+m,+a,+zve64x -passes='default<O2>' -S "$f.ll" -o "/tmp/$f.opt.ll"
  llc() { $B/llc -mtriple=rvtrim64 -mattr=+m,+a,+zve64x -rvtrim-v-vector-bits-min=128 -O2 "$@"; }
  vset=$(llc "/tmp/$f.opt.ll" -o - | grep -c $'\tvsetvli' || true)
  mnem=$(llc "/tmp/$f.opt.ll" -o - | grep -oE $'\t(v[a-z0-9]+\.[a-z0-9.]+)' | sort -u | tr '\n' ' ' | sed 's/\t//g' || true)
  insns=$(llc "/tmp/$f.opt.ll" -o - | grep -cE $'^\t[a-z]' || true)
  $B/opt -mtriple=rvtrim64 -mattr=+m,+a -passes='default<O2>' -S "$f.ll" -o "/tmp/$f.scalar.ll"
  sinsns=$($B/llc -mtriple=rvtrim64 -mattr=+m,+a -O2 "/tmp/$f.scalar.ll" -o - | grep -cE $'^\t[a-z]' || true)
  vec=$([ "$vset" -gt 0 ] && echo yes || echo no)
  printf '| %s | zve64x | %s | %s | %s | %s (scalar %s) |\n' "$f" "$vec" "$vset" "${mnem:-none}" "$insns" "$sinsns"
done
