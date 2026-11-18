#!/usr/bin/env bash
#
# Chapter 11 gate: the SIMT model is one source of truth.
#
# Usage: check-ch11.sh <llvm-bin-dir>
#
# What each group proves:
#   1. the JSON model is internally consistent (warp/block arithmetic, masks,
#      special-register names, address spaces);
#   2. every special-register funct in the model matches llvm-mc's encoding;
#   3. the seven intrinsics still lower to exactly those `sr.*` instructions;
#   4. the chapter-09 and chapter-11 docs still state the frozen numbers.
#
set -euo pipefail

BIN=${1:?usage: check-ch11.sh <llvm-bin-dir>}
HERE=$(cd "$(dirname "$0")" && pwd)
MODEL="$HERE/../tools/gpumin_model.py"
JSON="$HERE/simt-model.json"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

body() { # body <function> <asm-file>
  awk -v f="$1" '$0 ~ "^" f ":" {p=1} p{print} p && /^\.Lfunc_end/ {exit}' "$2"
}

# --- 1. model self-test ---------------------------------------------------
python3 "$MODEL" --selftest > /dev/null || fail "the SIMT model self-test failed"
echo "PASS 1/4: warp/block arithmetic, masks and special names are consistent"

# --- 2. model functs vs llvm-mc ------------------------------------------
names=$(python3 - "$JSON" <<'PY'
import json, sys
with open(sys.argv[1]) as f:
    data = json.load(f)
print(" ".join(r["name"] for r in data["special_registers"]))
PY
)

for name in $names; do
  want=$(python3 "$MODEL" --encoding "$name")
  got=$(printf 'sr.%s r1\n' "$name" |
        "$BIN/llvm-mc" -triple=gpumin -show-encoding |
        grep -o '\[[^]]*\]' | tr -d '[]')
  [ "$got" = "$want" ] ||
    fail "sr.$name encodes as [$got], model says [$want]"
done
echo "PASS 2/4: $(echo "$names" | wc -w) special functs match llvm-mc"

# --- 3. lowering still selects those instructions ------------------------
cat > "$TMP/model.ll" <<'IR'
target triple = "gpumin-unknown-elf"

declare i32 @llvm.gpumin.laneid()
declare i32 @llvm.gpumin.warpid()
declare i32 @llvm.gpumin.blockid()
declare i32 @llvm.gpumin.tid()
declare i32 @llvm.gpumin.ntid()
declare i32 @llvm.gpumin.nwarps()
declare i32 @llvm.gpumin.activemask()

define i32 @laneid() {
  %v = call i32 @llvm.gpumin.laneid()
  ret i32 %v
}
define i32 @warpid() {
  %v = call i32 @llvm.gpumin.warpid()
  ret i32 %v
}
define i32 @blockid() {
  %v = call i32 @llvm.gpumin.blockid()
  ret i32 %v
}
define i32 @tid() {
  %v = call i32 @llvm.gpumin.tid()
  ret i32 %v
}
define i32 @ntid() {
  %v = call i32 @llvm.gpumin.ntid()
  ret i32 %v
}
define i32 @nwarps() {
  %v = call i32 @llvm.gpumin.nwarps()
  ret i32 %v
}
define i32 @activemask() {
  %v = call i32 @llvm.gpumin.activemask()
  ret i32 %v
}
IR

"$BIN/llc" -march=gpumin -O1 -verify-machineinstrs "$TMP/model.ll" \
  -o "$TMP/model.s" || fail "llc rejected the model module"

for name in $names; do
  body "$name" "$TMP/model.s" | grep -q "sr.$name" ||
    fail "$name did not lower to sr.$name"
done
echo "PASS 3/4: every model name lowers to its own sr.* instruction"

# --- 4. docs state the frozen numbers ------------------------------------
python3 "$MODEL" --check-doc "$HERE/../docs/11-simt-model.md" > /dev/null ||
  fail "chapter 11 doc no longer matches the model"

while read -r funct name; do
  grep -q "| $funct | \`sr.$name\`" "$HERE/../docs/09-special-registers.md" ||
    fail "chapter 09 doc no longer maps funct $funct to sr.$name"
done < <(python3 - "$JSON" <<'PY'
import json, sys
with open(sys.argv[1]) as f:
    data = json.load(f)
for r in data["special_registers"]:
    print(r["funct"], r["name"])
PY
)
echo "PASS 4/4: chapters 09 and 11 agree with the JSON model"

echo "OK: chapter 11 gate passed"
