#!/usr/bin/env bash
# Nightly drift check for the hand-written X86 legalizer chain.
#
# The only failing condition is a source the reader cannot parse or a coverage
# gap that has no reviewed exception.  Query-domain differences between
# upstream/main and the frozen reference are expected (today: the CTPOP
# delta) and are reported, not failed.
#
# Usage: upstream-nightly.sh [ARTIFACT_DIR]
set -euo pipefail

tool_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=$(git -C "$tool_dir" rev-parse --show-toplevel)
tool="$tool_dir/__main__.py"
out=${1:-$(mktemp -d)}
mkdir -p "$out"

python3 "$tool" parse --repo "$repo" --source-rev upstream/main \
    --out "$out/upstream-ast.json"
python3 "$tool" points --repo "$repo" --source-rev upstream/main \
    --tier witness --out "$out/upstream-points.json"
python3 "$tool" corpus --repo "$repo" --source-rev upstream/main \
    --oracle=none --tier witness --out "$out/upstream-shapes.json"

witness="$repo/llvm/unittests/Target/X86/x86-legalizer-witness.json"
if [[ -f "$witness" ]]; then
  # Informational: a shapes-only corpus has no expected answers, and a
  # cross-revision domain difference is the point of the check.
  python3 "$tool" diff --left "$out/upstream-shapes.json" \
      --right "$witness" --out "$out/upstream-vs-witness.json"
fi

echo "nightly artifacts: $out"
