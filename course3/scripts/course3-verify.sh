#!/usr/bin/env bash
#
# Course 3 gate.  Every chapter adds checks here; chapter 00 has none yet,
# which is why the script prints "0 checks".
#
# Usage: course3-verify.sh <llvm-bin-dir> <rmach-binary>
#
# Chapter 01 checks:
#   * the kernel ELF's entry point and PT_LOAD list match llvm-readelf;
#   * _start runs and ebreak halts with the kernel's status;
#   * the first trace line is the reset vector 0x80000000.
#
# Chapter 02 checks:
#   * the kernel reads mhartid/misa and round-trips mscratch, exiting 0;
#   * --selftest passes every hand-computed CSR vector (22 at this chapter).
#
# Chapter 03 checks:
#   * six M-mode traps (ecall/ebreak/illegal/misaligned load/store/jalr) have
#     the exact (cause, epc, tval) the handler expects;
#   * the same handler takes ecall/ebreak/illegal from U-mode via mret.
#
# Chapter 04 checks:
#   * three CLINT timer interrupts in vectored mode (mcause=0x80000007,
#     mtval=0, mepc inside the wait loop, monotonic mtime);
#   * MIE+MTIE with an unreachable mtimecmp never fires.
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

build_asm_image() {
  local name=$1 src=$2
  "$CC" --target="$TRIPLE" -march=rv32im_zicsr -mabi=ilp32 -mno-relax \
    -c "$src" -o "$TMP/$name.o" || fail "clang could not assemble $src"
  "$LD" -m elf32lriscv -T "$COURSE3/kernel/link.ld" -o "$TMP/$name.elf" \
    "$TMP/$name.o" || fail "ld.lld could not link $src"
}

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

# _start must run: the chapter-02 kernel checks CSRs and returns 0.
set +e
"$RMACH" --kernel "$TMP/kernel.elf" > "$TMP/kernel.out" 2> "$TMP/kernel.err"
kernel_rc=$?
set -e
[[ "$kernel_rc" -eq 0 ]] || fail "kernel exited $kernel_rc, want 0"
grep -q 'instructions (exit 0)' "$TMP/kernel.err" \
  || fail "unexpected halt line: $(cat "$TMP/kernel.err")"
CHECKS=$((CHECKS + 1))

# The first trace line is the reset vector itself.
set +e
"$RMACH" --kernel "$TMP/kernel.elf" --trace > /dev/null 2> "$TMP/kernel.trace"
trace_rc=$?
set -e
[[ "$trace_rc" -eq 0 ]] || fail "traced run exited $trace_rc, want 0"
FIRST=$(awk 'NR == 1 { print $2 }' "$TMP/kernel.trace")
[[ "$FIRST" == "80000000" ]] || fail "first trace line is 0x$FIRST, want 0x80000000"
CHECKS=$((CHECKS + 1))

# The CSR file is also exercised without a kernel image.
"$RMACH" --selftest > "$TMP/selftest.out" || fail "--selftest failed"
grep -q '^selftest: 22 checks, 0 failures$' "$TMP/selftest.out" \
  || fail "unexpected selftest line: $(cat "$TMP/selftest.out")"
CHECKS=$((CHECKS + 1))

# --- Chapter 03: precise traps and mret ---------------------------------

build_asm_image traps "$COURSE3/tests/traps.s"
set +e
"$RMACH" --kernel "$TMP/traps.elf" > "$TMP/traps.out" 2> "$TMP/traps.err"
traps_rc=$?
set -e
[[ "$traps_rc" -eq 0 ]] || fail "traps.s exited $traps_rc (10*trap_index+stage)"
CHECKS=$((CHECKS + 1))

build_asm_image trap_u "$COURSE3/tests/trap-u.s"
set +e
"$RMACH" --kernel "$TMP/trap_u.elf" > "$TMP/trap_u.out" 2> "$TMP/trap_u.err"
trap_u_rc=$?
set -e
[[ "$trap_u_rc" -eq 0 ]] || fail "trap-u.s exited $trap_u_rc"
CHECKS=$((CHECKS + 1))

# --- Chapter 04: the CLINT and timer interrupts -------------------------

build_asm_image timer "$COURSE3/tests/timer.s"
set +e
"$RMACH" --kernel "$TMP/timer.elf" > "$TMP/timer.out" 2> "$TMP/timer.err"
timer_rc=$?
set -e
[[ "$timer_rc" -eq 0 ]] || fail "timer.s exited $timer_rc"
CHECKS=$((CHECKS + 1))

build_asm_image timer_off "$COURSE3/tests/timer-off.s"
set +e
"$RMACH" --kernel "$TMP/timer_off.elf" > "$TMP/timer_off.out" 2> "$TMP/timer_off.err"
timer_off_rc=$?
set -e
[[ "$timer_off_rc" -eq 0 ]] || fail "timer-off.s exited $timer_off_rc"
CHECKS=$((CHECKS + 1))

echo "$CHECKS checks"
