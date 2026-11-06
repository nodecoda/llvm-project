#!/usr/bin/env bash
# 断言：GlobalISel 不返回（超时）；SelectionDAG 同输入正常返回。
# 用法: check.sh <llc> [超时秒数，默认 60]
set -u
LLC=${1:?usage: check.sh <llc> [timeout-seconds]}
LIMIT=${2:-60}
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$HERE/mul-v2i32.ll

timeout "$LIMIT" "$LLC" -global-isel -O0 -mtriple=x86_64-linux-gnu "$SRC" -o /dev/null
rc_gisel=$?
if [ "$rc_gisel" -ne 124 ]; then
  echo "FAIL: global-isel rc=$rc_gisel, expected 124 (timeout after ${LIMIT}s)" >&2
  exit 1
fi

"$LLC" -O0 -mtriple=x86_64-linux-gnu "$SRC" -o /dev/null
rc_sdag=$?
if [ "$rc_sdag" -ne 0 ]; then
  echo "FAIL: selectiondag rc=$rc_sdag, expected 0" >&2
  exit 1
fi

echo "OK: global-isel rc=124 (${LIMIT}s), selectiondag rc=0"
