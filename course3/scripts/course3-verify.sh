#!/usr/bin/env bash
#
# Course 3 gate.  Every chapter adds checks here; chapter 00 has none yet,
# which is why the script prints "0 checks".
#
# Usage: course3-verify.sh <llvm-bin-dir> <rmach-binary>
#
# Chapter 01 checks:
#   * the kernel ELF's entry point and PT_LOAD list match llvm-readelf;
#   * _start runs, kmain returns 7 and ebreak halts with status 7;
#   * the first trace line is the reset vector 0x80000000.
#
set -euo pipefail

BIN=${1:?usage: course3-verify.sh <llvm-bin-dir> <rmach-binary>}
RMACH=${2:?usage: course3-verify.sh <llvm-bin-dir> <rmach-binary>}

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
COURSE3=$(cd "$HERE/.." && pwd)
CC="$BIN/clang"
READELF="$BIN/llvm-readelf"
LD=${LD:-ld.lld}
TRIPLE=riscv32-unknown-elf
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

[[ -x "$RMACH" ]] || fail "$RMACH is not executable"

# A smoke test, deliberately not counted as a chapter check.
"$RMACH" --version | grep -q '^rmach ' || fail "rmach --version is not rmach"

CHECKS=0

# --- Chapter 01: the bare-metal image and the reset vector --------------

build_kernel() {
  local name=$1
  "$CC" --target="$TRIPLE" -march=rv32im_zicsr -mabi=ilp32 -mno-relax \
    -ffreestanding -fno-builtin -O2 -c "$COURSE3/kernel/kmain.c" \
    -o "$TMP/$name-kmain.o" || fail "clang could not compile kmain.c"
  "$CC" --target="$TRIPLE" -march=rv32im_zicsr -mabi=ilp32 -mno-relax \
    -ffreestanding -fno-builtin -O2 -c "$COURSE3/kernel/start.S" \
    -o "$TMP/$name-start.o" || fail "clang could not assemble start.S"
  "$LD" -m elf32lriscv -T "$COURSE3/kernel/link.ld" -o "$TMP/$name.elf" \
    "$TMP/$name-start.o" "$TMP/$name-kmain.o" \
    || fail "ld.lld could not link the kernel"
}
build_kernel kernel

# The loader must see the same entry point and PT_LOAD list as llvm-readelf.
python3 - "$READELF" "$RMACH" "$TMP/kernel.elf" <<'PY'
import re, subprocess, sys
readelf, rmach, elf = sys.argv[1:4]
rl = subprocess.run([readelf, "-hl", elf], capture_output=True, text=True,
                    check=True).stdout
ro = subprocess.run([rmach, "--kernel", elf, "--dump-boot"],
                    capture_output=True, text=True, check=True).stdout
m = re.search(r"Entry point address:\s+0x([0-9a-f]+)", rl)
o = re.search(r"^entry 0x([0-9a-f]+)$", ro, re.M)
if not m or not o or int(m.group(1), 16) != int(o.group(1), 16):
    sys.exit("entry point differs")
segs = []
for line in rl.splitlines():
    if line.strip().startswith("LOAD"):
        p = line.split()
        segs.append((int(p[2], 16), int(p[4], 16), int(p[5], 16)))
ours = []
for line in ro.splitlines():
    if line.startswith("load "):
        f = dict(kv.split("=") for kv in line.split()[1:])
        ours.append((int(f["vaddr"], 16), int(f["filesz"], 16),
                     int(f["memsz"], 16)))
if segs != ours:
    sys.exit("PT_LOAD list differs: %r vs %r" % (segs, ours))
if int(o.group(1), 16) != 0x80000000:
    sys.exit("kernel is not linked at 0x80000000")
PY
CHECKS=$((CHECKS + 1))

# _start must run: kmain returns 7, so ebreak halts with status 7.
set +e
"$RMACH" --kernel "$TMP/kernel.elf" > "$TMP/kernel.out" 2> "$TMP/kernel.err"
kernel_rc=$?
set -e
[[ "$kernel_rc" -eq 7 ]] || fail "kernel exited $kernel_rc, want 7"
grep -q 'halted at 0x80000010 after 6 instructions (exit 7)' "$TMP/kernel.err" \
  || fail "unexpected halt line: $(cat "$TMP/kernel.err")"
CHECKS=$((CHECKS + 1))

# The first trace line is the reset vector itself.
set +e
"$RMACH" --kernel "$TMP/kernel.elf" --trace > /dev/null 2> "$TMP/kernel.trace"
trace_rc=$?
set -e
[[ "$trace_rc" -eq 7 ]] || fail "traced run exited $trace_rc, want 7"
FIRST=$(awk 'NR == 1 { print $2 }' "$TMP/kernel.trace")
[[ "$FIRST" == "80000000" ]] || fail "first trace line is 0x$FIRST, want 0x80000000"
CHECKS=$((CHECKS + 1))

echo "$CHECKS checks"
