#!/usr/bin/env bash
# 一条命令跑完所有用例，并把"这次跑的是哪棵树、哪个 llc"记进 _out/snapshot.txt。
# 用法: run.sh [path/to/llc]   （默认取 $LLC）
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
LLC=${1:-${LLC:-}}
if [ -z "$LLC" ] || [ ! -x "$LLC" ]; then
  echo "usage: run.sh /path/to/builds/<task>/bin/llc" >&2
  exit 2
fi
BIN=$(dirname "$LLC")
REPO=$(cd "$HERE/.." && pwd)
mkdir -p "$HERE/_out"

{
  echo "date:  $(date -Iseconds)"
  echo "llc:   $LLC"
  "$LLC" --version | head -1
  echo "llvm:  $(git -C "$REPO" rev-parse HEAD 2>/dev/null || echo unknown)"
  echo "dirty: $(git -C "$REPO" status --porcelain | wc -l)"
} > "$HERE/_out/snapshot.txt"

echo "== snapshot" && sed 's/^/   /' "$HERE/_out/snapshot.txt"
python3 "$REPO/llvm/utils/lit/lit.py" -s \
  --param llc="$LLC" --param filecheck="$BIN/FileCheck" "$HERE" | tail -5
