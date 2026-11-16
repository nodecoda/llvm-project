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
# Chapter 07 checks:
#   * the course-2 demo runs as a U-mode program: stdout "rvmin: ok\n", exit 0;
#   * a write buffer that crosses a page boundary is copied byte by byte;
#   * --dump-pagetable shows the demo text page, the UART and the user stack.
#
# Chapter 08 checks:
#   * two U-mode processes alternate: 300 "A" lines and 200 "B" lines, exit 7;
#   * --stats proves the timer fired and both processes made progress;
#   * --dump-procs reports both EXITED with exit codes 7 and 0;
#   * the two processes' output is interleaved, not serialized.
#
# Chapter 09 checks:
#   * --trace has one line per retired instruction, equal to --stats and
#     minstret;
#   * --dump-csr reports the hand-computed misa/mhartid/satp values;
#   * a U-mode trace line carries the page-table translation 0x10000->0x80400000.
#
# Chapter 10 checks:
#   * all 31 GPRs survive a U ecall -> M handler -> mret round trip;
#   * the course-2 muldiv_i program (rvmin, no M extension) runs as a U process.
#
set -euo pipefail

BIN=${1:?usage: course3-verify.sh <llvm-bin-dir> <rmach-binary>}
RMACH=${2:?usage: course3-verify.sh <llvm-bin-dir> <rmach-binary>}

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
COURSE3=$(cd "$HERE/.." && pwd)
CC="$BIN/clang"
READELF="$BIN/llvm-readelf"
LLC="$BIN/llc"
MC="$BIN/llvm-mc"
LD=${LD:-ld.lld}
TRIPLE=riscv32-unknown-elf
RT="$COURSE3/../llvm/lib/Target/RVMin/scripts/rt"
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

# The chapter-07 kernel is four C files plus start.S and trap.S.  The generic
# build_c_image above stays for standalone test kernels such as uart-echo.c.
build_kernel() {
  local name=kernel f
  for f in kmain uart mmu exit proc string; do
    "$CC" "${KFLAGS[@]}" -c "$COURSE3/kernel/$f.c" -o "$TMP/$name-$f.o" \
      || fail "clang could not compile kernel/$f.c"
  done
  "$CC" "${AFLAGS[@]}" -c "$COURSE3/kernel/start.S" -o "$TMP/$name-start.o" \
    || fail "clang could not assemble start.S"
  "$CC" "${AFLAGS[@]}" -c "$COURSE3/kernel/trap.S" -o "$TMP/$name-trap.o" \
    || fail "clang could not assemble trap.S"
  "$LD" -m elf32lriscv -T "$COURSE3/kernel/link.ld" -o "$TMP/$name.elf" \
    "$TMP/$name-start.o" "$TMP/$name-trap.o" "$TMP/$name-kmain.o" \
    "$TMP/$name-uart.o" "$TMP/$name-mmu.o" "$TMP/$name-exit.o" \
    "$TMP/$name-proc.o" "$TMP/$name-string.o" \
    || fail "ld.lld could not link the kernel"
}
build_kernel

build_asm_image() {
  local name=$1 src=$2
  "$CC" "${AFLAGS[@]}" -c "$src" -o "$TMP/$name.o" \
    || fail "clang could not assemble $src"
  "$LD" -m elf32lriscv -T "$COURSE3/kernel/link.ld" -o "$TMP/$name.elf" \
    "$TMP/$name.o" || fail "ld.lld could not link $src"
}

# Build a course-2 program with the course-1 backend.  Chapter 07 feeds the
# result to --user, so the three courses meet in one command.
build_rvmin_program() {
  local name=$1 src=$2 opt=$3
  "$CC" --target="$TRIPLE" -march=rv32i -mabi=ilp32 -O"$opt" -ffreestanding \
    -fno-builtin -S -emit-llvm "$src" -o "$TMP/$name.ll" \
    || fail "clang could not compile $src"
  "$LLC" -march=rvmin -mtriple="$TRIPLE" -O"$opt" "$TMP/$name.ll" \
    -o "$TMP/$name.s" || fail "llc could not select $name"
  "$MC" -triple=rvmin -filetype=obj "$TMP/$name.s" -o "$TMP/$name.o" \
    || fail "llvm-mc rejected $name"
  "$MC" -triple=rvmin -filetype=obj "$RT/rvmin-rt.S" \
    -o "$TMP/rt-$name.o" || fail "llvm-mc rejected rvmin-rt.S"
  "$MC" -triple=rvmin -filetype=obj "$RT/start.S" \
    -o "$TMP/start-$name.o" || fail "llvm-mc rejected start.S"
  "$LD" -m elf32lriscv -T "$RT/link.ld" -o "$TMP/$name" \
    "$TMP/start-$name.o" "$TMP/$name.o" "$TMP/rt-$name.o" \
    || fail "ld.lld could not link $name"
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

# --- Chapter 07: U-mode programs and system calls -----------------------

# The course-2 demo becomes the first real U-mode program: rvmin compiled it,
# the kernel mapped it, and the write/exit syscalls carry its output.
build_rvmin_program demo "$RT/demo.c" 2
printf 'rvmin: ok\n' > "$TMP/demo.golden"
set +e
"$RMACH" --kernel "$TMP/kernel.elf" --user "$TMP/demo" \
  > "$TMP/demo.out" 2> "$TMP/demo.err"
demo_rc=$?
set -e
[[ "$demo_rc" -eq 0 ]] || fail "demo under rmach exited $demo_rc, want 0"
cmp -s "$TMP/demo.out" "$TMP/demo.golden" \
  || fail "demo stdout is $(od -An -tx1c "$TMP/demo.out" | tr '\n' ' ')"
CHECKS=$((CHECKS + 1))

# A 24-byte buffer starts six bytes before a page boundary.  A kernel that
# translates only the first page loses the tail.
"$CC" --target="$TRIPLE" -march=rv32i -mabi=ilp32 -mno-relax \
  -c "$COURSE3/tests/user-crosspage.S" -o "$TMP/cross.o" \
  || fail "clang could not assemble user-crosspage.S"
"$LD" -m elf32lriscv -T "$RT/link.ld" -o "$TMP/cross" "$TMP/cross.o" \
  || fail "ld.lld could not link user-crosspage"
printf '0123456789abcdefghijklmn' > "$TMP/cross.golden"
set +e
"$RMACH" --kernel "$TMP/kernel.elf" --user "$TMP/cross" \
  > "$TMP/cross.out" 2> "$TMP/cross.err"
cross_rc=$?
set -e
[[ "$cross_rc" -eq 0 ]] || fail "user-crosspage exited $cross_rc, want 0"
cmp -s "$TMP/cross.out" "$TMP/cross.golden" \
  || fail "user-crosspage stdout is $(od -An -tx1c "$TMP/cross.out" | tr '\n' ' ')"
CHECKS=$((CHECKS + 1))

# The U-mode address space comes from boot_info: one demo page (its R, RW and
# X segments share a page, so the flags are their union), four stack pages and
# the console.
set +e
"$RMACH" --kernel "$TMP/kernel.elf" --user "$TMP/demo" --dump-pagetable \
  > "$TMP/demo.dump" 2> /dev/null
demo_dump_rc=$?
set -e
[[ "$demo_dump_rc" -eq 0 ]] || fail "demo --dump-pagetable exited $demo_dump_rc"
grep -q '^leaf va=0x00010000 pa=0x80400000 .* flags=VRWXUAD$' "$TMP/demo.dump" \
  || fail "dump lacks the demo text page: $(cat "$TMP/demo.dump")"
grep -q '^leaf va=0x10000000 pa=0x10000000 .* flags=VRWUAD$' "$TMP/demo.dump" \
  || fail "dump lacks the UART page: $(cat "$TMP/demo.dump")"
grep -q '^leaf va=0x7feff000 pa=0x80423000 .* flags=VRWUAD$' "$TMP/demo.dump" \
  || fail "dump lacks the last stack page: $(cat "$TMP/demo.dump")"
CHECKS=$((CHECKS + 1))

# --- Chapter 08: two processes and round-robin scheduling ---------------

build_proc() {
  local name=$1
  "$CC" --target="$TRIPLE" -march=rv32i -mabi=ilp32 -mno-relax \
    -c "$COURSE3/tests/$name.S" -o "$TMP/$name.o" \
    || fail "clang could not assemble $name.S"
  "$LD" -m elf32lriscv -T "$RT/link.ld" -o "$TMP/$name" "$TMP/$name.o" \
    || fail "ld.lld could not link $name"
}
build_proc proc-a
build_proc proc-b

# Both processes print one line per write syscall, so a corrupt context switch
# shows up as a missing line, a stray byte, or the wrong process order.
set +e
"$RMACH" --kernel "$TMP/kernel.elf" --user "$TMP/proc-a" --user "$TMP/proc-b" \
  --stats > "$TMP/proc.out" 2> "$TMP/proc.err"
proc_rc=$?
set -e
[[ "$proc_rc" -eq 7 ]] || fail "two-process run exited $proc_rc, want 7"
awk 'BEGIN { a = 0; b = 0; bad = 0 }
     $0 == "A" { a++ }
     $0 == "B" { b++ }
     $0 != "A" && $0 != "B" { bad = 1 }
     END { if (bad || a != 300 || b != 200) exit 1 }' "$TMP/proc.out" \
  || fail "two-process output is not 300 A + 200 B lines"
CHECKS=$((CHECKS + 1))

# The timer must have preempted the U-mode programs, and every write/exit
# syscall must have gone through the handler.
grep -q 'timer_ticks=' "$TMP/proc.err" \
  || fail "no stats line: $(cat "$TMP/proc.err")"
grep -Eq 'timer_ticks=([4-9]|[1-9][0-9]+)' "$TMP/proc.err" \
  || fail "timer did not fire at least four times: $(cat "$TMP/proc.err")"
grep -q 'ecalls=502' "$TMP/proc.err" \
  || fail "expected 300 write + 200 write + 2 exit ecalls: $(cat "$TMP/proc.err")"
CHECKS=$((CHECKS + 1))

# The kernel publishes its process table at BOOT_RESULT_ADDR; both processes
# ran to completion and their exit codes were collected independently.
set +e
"$RMACH" --kernel "$TMP/kernel.elf" --user "$TMP/proc-a" --user "$TMP/proc-b" \
  --dump-procs > "$TMP/procs.dump" 2> /dev/null
procs_rc=$?
set -e
[[ "$procs_rc" -eq 7 ]] || fail "two-process --dump-procs exited $procs_rc"
grep -q '^nproc 2$' "$TMP/procs.dump" || fail "dump lacks nproc 2"
grep -q '^proc 0 state=EXITED exit=7 ' "$TMP/procs.dump" \
  || fail "dump lacks process 0 exit 7: $(cat "$TMP/procs.dump")"
grep -q '^proc 1 state=EXITED exit=0 ' "$TMP/procs.dump" \
  || fail "dump lacks process 1 exit 0: $(cat "$TMP/procs.dump")"
CHECKS=$((CHECKS + 1))

# Interleaving: B must start before A prints its 300th line, and A must start
# before B prints its 200th.  Serial execution would fail one of these.
awk 'BEGIN { firstA = 0; firstB = 0 }
     $0 == "A" && firstA == 0 { firstA = NR }
     $0 == "B" && firstB == 0 { firstB = NR }
     END { if (firstA == 0 || firstB == 0 || firstA >= 200 || firstB >= 300)
             exit 1 }' "$TMP/proc.out" \
  || fail "process output is not interleaved: $(head -20 "$TMP/proc.out" | tr '\n' ' ')"
CHECKS=$((CHECKS + 1))

# --- Chapter 09: trace, CSR dump and page-table selection ---------------

# Every retired instruction is one trace line.  The same number must appear in
# --stats and in minstret; if any of the three drifts, one of them is lying.
set +e
"$RMACH" --kernel "$TMP/kernel.elf" --user "$TMP/demo" --trace --stats \
  --dump-csr > "$TMP/trace.out" 2> "$TMP/trace.err"
trace_rc=$?
set -e
[[ "$trace_rc" -eq 0 ]] || fail "trace run exited $trace_rc, want 0"
trace_lines=$(grep -c '^T ' "$TMP/trace.err" || true)
instr=$(sed -n 's/^rmach: stats: instructions=\([0-9][0-9]*\) .*/\1/p' \
  "$TMP/trace.err")
minstret_hex=$(sed -n 's/^csr minstret=0x\([0-9a-f]*\)$/\1/p' \
  "$TMP/trace.out")
[[ -n "$instr" ]] || fail "no stats line in $(cat "$TMP/trace.err")"
[[ -n "$minstret_hex" ]] || fail "no minstret line in $(cat "$TMP/trace.out")"
[[ "$trace_lines" -eq "$instr" ]] \
  || fail "trace has $trace_lines lines, stats says $instr"
[[ "$trace_lines" -eq "$((16#$minstret_hex))" ]] \
  || fail "trace has $trace_lines lines, minstret is 0x$minstret_hex"
CHECKS=$((CHECKS + 1))

# These are the values the chapter's CSR table promises.  satp has MODE=1 in
# bit 31, so a simple prefix match is enough.
grep -q '^csr misa=0x40001100$' "$TMP/trace.out" \
  || fail "--dump-csr lost misa: $(grep '^csr misa=' "$TMP/trace.out")"
grep -q '^csr mhartid=0x00000000$' "$TMP/trace.out" \
  || fail "--dump-csr lost mhartid"
grep -q '^csr satp=0x800' "$TMP/trace.out" \
  || fail "--dump-csr did not report Sv32 satp"
CHECKS=$((CHECKS + 1))

# The first U-mode instruction is the demo's _start at VA 0x10000.  Its fetch
# PA must be the first physical page of the user arena.
grep -q '^T 00010000 80400000 U ' "$TMP/trace.err" \
  || fail "trace does not translate the first U instruction: $(grep '^T .* U ' "$TMP/trace.err" | head -2)"
CHECKS=$((CHECKS + 1))

# --- Chapter 10: acceptance evidence ------------------------------------

# C3-AC2: the trap frame covers all 31 GPRs.  The test snapshots every
# register before ecall, snapshots again after mret and compares the words.
build_asm_image trap_allregs "$COURSE3/tests/trap-allregs.S"
set +e
"$RMACH" --kernel "$TMP/trap_allregs.elf" > "$TMP/allregs.out" 2> "$TMP/allregs.err"
allregs_rc=$?
set -e
[[ "$allregs_rc" -eq 0 ]] || fail "trap-allregs exited $allregs_rc (a GPR changed)"
[[ ! -s "$TMP/allregs.out" ]] || fail "trap-allregs produced unexpected stdout"
CHECKS=$((CHECKS + 1))

# C3-AC8: the second course's muldiv_i is built with rvmin (no +m, so it uses
# the rvmin-rt libcalls) and runs as a U-mode process under the same kernel.
build_rvmin_program muldiv_i "$RT/muldiv.c" 2
printf 'rvmin: ok\n' > "$TMP/muldiv.golden"
set +e
"$RMACH" --kernel "$TMP/kernel.elf" --user "$TMP/muldiv_i" \
  > "$TMP/muldiv.out" 2> "$TMP/muldiv.err"
muldiv_rc=$?
set -e
[[ "$muldiv_rc" -eq 0 ]] || fail "muldiv_i under rmach exited $muldiv_rc, want 0"
cmp -s "$TMP/muldiv.out" "$TMP/muldiv.golden" \
  || fail "muldiv_i stdout is $(od -An -tx1c "$TMP/muldiv.out" | tr '\n' ' ')"
CHECKS=$((CHECKS + 1))

echo "$CHECKS checks"
