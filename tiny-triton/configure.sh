#!/usr/bin/env bash
# Reconfigure an existing task build so that the tiny-triton external project
# shows up in the build graph.  Incremental: only these cache entries change.
#
#   tiny-triton/configure.sh [repo] [build]
#
set -eu

REPO=${1:-$(cd "$(dirname "$0")/.." && pwd)}
BUILD=${2:-/home/ubuntu/llvm/builds/$(basename "$REPO")}

[ -d "$REPO/llvm" ] || { echo "no llvm/ under $REPO" >&2; exit 2; }
[ -f "$BUILD/CMakeCache.txt" ] || { echo "no configured build at $BUILD" >&2; exit 2; }

# Guard: this build deliberately excludes NVPTX (disk budget).  A reconfigure
# that silently restores toolset/configure.sh's default would add it back.
targets=$(grep -E '^LLVM_TARGETS_TO_BUILD:STRING=' "$BUILD/CMakeCache.txt" | cut -d= -f2-)
case "$targets" in
  *NVPTX*) echo "refusing to reconfigure: LLVM_TARGETS_TO_BUILD=$targets" >&2; exit 3 ;;
esac

exec cmake -S "$REPO/llvm" -B "$BUILD" \
  -DLLVM_EXTERNAL_PROJECTS=tiny-triton \
  -DLLVM_EXTERNAL_TINY_TRITON_SOURCE_DIR="$REPO/tiny-triton"
