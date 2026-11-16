#!/usr/bin/env bash
#
# Course 3 gate.  Every chapter adds checks here; chapter 00 has none yet,
# which is why the script prints "0 checks".
#
# Usage: course3-verify.sh <llvm-bin-dir> <rmach-binary>
#
# Chapter 01 checks:
#   * the kernel ELF's entry point and PT_LOAD list match llvm-readelf;
#   * the kernel reaches the test finisher and exits 0;
#   * the first trace line is the reset vector 0x80000000.
#
# Chapter 02 checks:
#   * --selftest passes every hand-computed vector (32 at chapter 06).
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
# Chapter 05 checks:
#   * the kernel's UART stdout is exactly "hello, rmach\n";
#   * --input drives RBR and the echo stops at the first newline;
#   * a store of 0x00075555 to the test word halts with exit code 7.
#
# Chapter 06 checks:
#   * the same U blob prints "U ok\n" under an identity and a relocated map;
#   * --dump-pagetable shows the relocated U page and the UART leaf;
#   * a U load from a U=0 kernel page traps with cause 13.
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

KFLAGS=(--target="$TRIPLE" -march=rv32im_zicsr -mabi=ilp32 -mno-relax
        -ffreestanding -fno-builtin -O2 -msmall-data-limit=0)
AFLAGS=(--target="$TRIPLE" -march=rv32im_zicsr -mabi=ilp32 -mno-relax)

build_c_image() {
  local name=$1 src=$2
  "$CC" "${KFLAGS[@]}" -c "$src" -o "$TMP/$name-main.o" \
    || fail "clang could not compile $src"
  "$CC" "${AFLAGS[@]}" -c "$COURSE3/kernel/start.S" \
    -o "$TMP/$name-start.o" || fail "clang could not assemble start.S"
  "$LD" -m elf32lriscv -T "$COURSE3/kernel/link.ld" -o "$TMP/$name.elf" \
    "$TMP/$name-start.o" "$TMP/$name-main.o" \
    || fail "ld.lld could not link $src"
}

# An MMU test needs a C kernel, a U-mode blob and the shared trap trampoline.
build_mmu_image() {
  local name=$1 main=$2 blob=$3
  "$CC" "${KFLAGS[@]}" -c "$main" -o "$TMP/$name-main.o" \
    || fail "clang could not compile $main"
  "$CC" "${KFLAGS[@]}" -c "$blob" -o "$TMP/$name-blob.o" \
    || fail "clang could not assemble $blob"
  "$CC" "${KFLAGS[@]}" -c "$COURSE3/tests/trap-entry.S" \
    -o "$TMP/$name-trap.o" || fail "clang could not assemble trap-entry.S"
  "$CC" "${AFLAGS[@]}" -c "$COURSE3/kernel/start.S" \
    -o "$TMP/$name-start.o" || fail "clang could not assemble start.S"
  "$LD" -m elf32lriscv -T "$COURSE3/kernel/link.ld" -o "$TMP/$name.elf" \
    "$TMP/$name-start.o" "$TMP/$name-main.o" "$TMP/$name-blob.o" \
    "$TMP/$name-trap.o" || fail "ld.lld could not link $main"
}

build_kernel() { build_c_image kernel "$COURSE3/kernel/kmain.c"; }
build_kernel

build_asm_image() {
  local name=$1 src=$2
  "$CC" "${AFLAGS[@]}" -c "$src" -o "$TMP/$name.o" \
    || fail "clang could not assemble $src"
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

# _start must run.  At chapter 05 the kernel prints hello and finishes
# through the test word with exit code 0.
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
grep -q '^selftest: 32 checks, 0 failures$' "$TMP/selftest.out" \
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

# --- Chapter 05: the bus, the UART and the test finisher ----------------

# The kernel's stdout is the UART, so compare it byte for byte, not with grep.
printf 'hello, rmach\n' > "$TMP/hello.golden"
set +e
"$RMACH" --kernel "$TMP/kernel.elf" > "$TMP/hello.out" 2> "$TMP/hello.err"
hello_rc=$?
set -e
[[ "$hello_rc" -eq 0 ]] || fail "hello exited $hello_rc, want 0"
cmp -s "$TMP/hello.out" "$TMP/hello.golden" \
  || fail "hello stdout is $(od -An -tx1c "$TMP/hello.out" | tr '\n' ' ')"
CHECKS=$((CHECKS + 1))

# RX comes from --input; the program stops at the first newline.
build_c_image uart_echo "$COURSE3/tests/uart-echo.c"
printf 'ping 42\nignored\n' > "$TMP/echo.in"
printf 'ping 42\n' > "$TMP/echo.golden"
set +e
"$RMACH" --kernel "$TMP/uart_echo.elf" --input "$TMP/echo.in" \
  > "$TMP/echo.out" 2> "$TMP/echo.err"
echo_rc=$?
set -e
[[ "$echo_rc" -eq 0 ]] || fail "uart-echo exited $echo_rc, want 0"
cmp -s "$TMP/echo.out" "$TMP/echo.golden" \
  || fail "uart-echo stdout is $(od -An -tx1c "$TMP/echo.out" | tr '\n' ' ')"
CHECKS=$((CHECKS + 1))

# A store to the test word halts the machine; the upper half is the exit code.
build_asm_image finisher "$COURSE3/tests/finisher.s"
set +e
"$RMACH" --kernel "$TMP/finisher.elf" > /dev/null 2> "$TMP/finisher.err"
finisher_rc=$?
set -e
[[ "$finisher_rc" -eq 7 ]] || fail "finisher.s exited $finisher_rc, want 7"
CHECKS=$((CHECKS + 1))

# --- Chapter 06: Sv32 translation and page faults -----------------------

# The same U blob runs twice: first at VA 0x80001000, then at VA 0x00010000;
# both map PA 0x80400000, so stdout must contain the sentence twice.
build_mmu_image mmu_run "$COURSE3/tests/mmu-run.c" "$COURSE3/tests/u-blob.S"
printf 'U ok\nU ok\n' > "$TMP/mmu.golden"
set +e
"$RMACH" --kernel "$TMP/mmu_run.elf" > "$TMP/mmu.out" 2> "$TMP/mmu.err"
mmu_rc=$?
set -e
[[ "$mmu_rc" -eq 0 ]] || fail "mmu-run exited $mmu_rc, want 0"
cmp -s "$TMP/mmu.out" "$TMP/mmu.golden" \
  || fail "mmu-run stdout is $(od -An -tx1c "$TMP/mmu.out" | tr '\n' ' ')"
CHECKS=$((CHECKS + 1))

# The final satp points at the relocated root: two leaves, one U code page and
# one UART page.  This is the hand-computed table the chapter walks through.
set +e
"$RMACH" --kernel "$TMP/mmu_run.elf" --dump-pagetable \
  > "$TMP/mmu.dump" 2> /dev/null
dump_rc=$?
set -e
[[ "$dump_rc" -eq 0 ]] || fail "mmu-run --dump-pagetable exited $dump_rc"
grep -q '^satp 0x[0-9a-f]* mode=1 ' "$TMP/mmu.dump" \
  || fail "dump has no mode=1 satp line: $(cat "$TMP/mmu.dump")"
grep -q '^leaf va=0x00010000 pa=0x80400000 .* flags=VRXUA$' "$TMP/mmu.dump" \
  || fail "dump lacks the relocated U page: $(cat "$TMP/mmu.dump")"
grep -q '^leaf va=0x10000000 pa=0x10000000 .* flags=VRWUAD$' "$TMP/mmu.dump" \
  || fail "dump lacks the UART page: $(cat "$TMP/mmu.dump")"
CHECKS=$((CHECKS + 1))

# U loads from 0x80000000, which is mapped R/W/X but U=0: the handler only
# accepts cause 13, and a 0x7777 failure word would exit 1.
build_mmu_image mmu_fault "$COURSE3/tests/mmu-fault.c" "$COURSE3/tests/u-fault.S"
set +e
"$RMACH" --kernel "$TMP/mmu_fault.elf" > "$TMP/mmu_fault.out" 2> "$TMP/mmu_fault.err"
fault_rc=$?
set -e
[[ "$fault_rc" -eq 0 ]] || fail "mmu-fault exited $fault_rc (cause was not 13)"
[[ ! -s "$TMP/mmu_fault.out" ]] || fail "mmu-fault produced unexpected stdout"
CHECKS=$((CHECKS + 1))

echo "$CHECKS checks"
