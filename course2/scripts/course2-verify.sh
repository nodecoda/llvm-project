#!/usr/bin/env bash
#
# Course 2 gate.
#
# Usage: course2-verify.sh <llvm-bin-dir> <rsim-binary>
#
# Chapter 01 checks:
#   * five executables built by the first course load with an entry point and
#     a PT_LOAD list identical to llvm-readelf's;
#   * `rsim FILE` loads the image and reports it;
#   * truncated headers, bad magic, non-ET_EXEC input and missing files all
#     fail with exit code 125 and a one-line diagnostic.
#
set -euo pipefail

BIN=${1:?usage: course2-verify.sh <llvm-bin-dir> <rsim-binary>}
RSIM=${2:?usage: course2-verify.sh <llvm-bin-dir> <rsim-binary>}

CC="$BIN/clang"
LLC="$BIN/llc"
MC="$BIN/llvm-mc"
READELF="$BIN/llvm-readelf"
LD=${LD:-ld.lld}

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
COURSE2=$(cd "$HERE/.." && pwd)
RT="$COURSE2/../llvm/lib/Target/RVMin/scripts/rt"
TRIPLE=riscv32-unknown-elf
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
CHECKS=0

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

[[ -x "$RSIM" ]] || fail "$RSIM is not executable"
[[ -d "$RT" ]] || fail "cannot find the course-1 runtime at $RT"

# build_program <name> <source> <opt> [mattr]
build_program() {
  local name=$1 src=$2 opt=$3 mattr=${4:-}
  local llc_args=(-march=rvmin -mtriple="$TRIPLE" -O"$opt")
  local mc_args=(-triple=rvmin)
  [[ -n "$mattr" ]] && { llc_args+=(-mattr="$mattr"); mc_args+=(-mattr="$mattr"); }

  "$CC" --target="$TRIPLE" -march=rv32i -mabi=ilp32 -O"$opt" -ffreestanding \
    -fno-builtin -S -emit-llvm "$src" -o "$TMP/$name.ll" \
    || fail "clang could not compile $src"
  "$LLC" "${llc_args[@]}" "$TMP/$name.ll" -o "$TMP/$name.s" \
    || fail "llc could not select $name"
  "$MC" "${mc_args[@]}" -filetype=obj "$TMP/$name.s" -o "$TMP/$name.o" \
    || fail "llvm-mc rejected $name"
  "$MC" "${mc_args[@]}" -filetype=obj "$RT/rvmin-rt.S" -o "$TMP/rt-$name.o" \
    || fail "llvm-mc rejected rvmin-rt.S"
  "$MC" "${mc_args[@]}" -filetype=obj "$RT/start.S" -o "$TMP/start-$name.o" \
    || fail "llvm-mc rejected start.S"
  "$LD" -m elf32lriscv -T "$RT/link.ld" -o "$TMP/$name" \
    "$TMP/start-$name.o" "$TMP/$name.o" "$TMP/rt-$name.o" \
    || fail "ld.lld could not link $name"
}

cat > "$TMP/compare.py" <<'PY'
import re
import subprocess
import sys

readelf, rsim, elf = sys.argv[1:4]
rl = subprocess.run([readelf, "-hl", elf], capture_output=True, text=True,
                    check=True).stdout
ro = subprocess.run([rsim, "--dump-elf", elf], capture_output=True, text=True,
                    check=True).stdout

entry = int(re.search(r"Entry point address:\s*(0x[0-9a-fA-F]+)", rl).group(1),
            16)
ref = []
for line in rl.splitlines():
    m = re.match(
        r"\s*LOAD\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+"
        r"(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+(.{3})\s+(0x[0-9a-f]+)", line)
    if not m:
        continue
    _, vaddr, _, filesz, memsz, flg, align = m.groups()
    flags = (4 if "R" in flg else 0) | (2 if "W" in flg else 0) | \
            (1 if "E" in flg else 0)
    ref.append((int(vaddr, 16), int(filesz, 16), int(memsz, 16), flags,
                int(align, 16)))

lines = ro.splitlines()
if not lines or lines[0] != "entry 0x%08x" % entry:
    sys.exit("entry mismatch: rsim=%r readelf=0x%08x" % (lines[:1], entry))

got = []
for line in lines[1:]:
    m = re.match(r"load vaddr=0x([0-9a-f]+) filesz=0x([0-9a-f]+) "
                 r"memsz=0x([0-9a-f]+) flags=0x([0-9a-f]+) align=0x([0-9a-f]+)",
                 line)
    if not m:
        sys.exit("unexpected rsim line: %r" % line)
    got.append(tuple(int(g, 16) for g in m.groups()))

if got != ref:
    sys.exit("segment mismatch:\n  rsim    = %r\n  readelf = %r" % (got, ref))
PY

compare() {
  python3 "$TMP/compare.py" "$READELF" "$RSIM" "$1" \
    || fail "ELF header/segments differ for $1"
  CHECKS=$((CHECKS + 1))
}

build_program demo "$RT/demo.c" 2
build_program muldiv_m "$RT/muldiv.c" 2 +m
build_program large_frame "$RT/large-frame.c" 2
build_program tail "$RT/tail.c" 2
build_program switch "$RT/switch.c" 2

for name in demo muldiv_m large_frame tail switch; do
  compare "$TMP/$name"
done

# Plain loading reports the same entry point.
"$RSIM" "$TMP/demo" > "$TMP/load.txt" || fail "rsim could not load demo"
grep -q 'entry 0x00010000' "$TMP/load.txt" || fail "load output lost the entry"
CHECKS=$((CHECKS + 1))

# Rejections: each must exit 125 with an rsim: diagnostic.
expect_reject() {
  local name=$1 file=$2
  set +e
  "$RSIM" --dump-elf "$file" > "$TMP/$name.out" 2> "$TMP/$name.err"
  local rc=$?
  set -e
  [[ "$rc" -eq 125 ]] || fail "$name: exit code $rc, want 125"
  grep -q '^rsim: ' "$TMP/$name.err" || fail "$name: no rsim: diagnostic"
  CHECKS=$((CHECKS + 1))
}

head -c 20 "$TMP/demo" > "$TMP/truncated"
printf 'not an elf file' > "$TMP/bad-magic"
expect_reject truncated "$TMP/truncated"
expect_reject bad-magic "$TMP/bad-magic"
expect_reject et-rel "$TMP/demo.o"
expect_reject missing "$TMP/does-not-exist"

echo "$CHECKS checks"
