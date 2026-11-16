#!/usr/bin/env bash
#
# Course 2 gate.
#
# Usage: course2-verify.sh <llvm-bin-dir> <rsim-binary>
#
# Chapter 01-02 checks:
#   * five executables built by the first course load with an entry point and
#     a PT_LOAD list identical to llvm-readelf's;
#   * `rsim FILE` loads the image and reports it;
#   * truncated headers, bad magic, non-ET_EXEC input and missing files all
#     fail with exit code 125 and a one-line diagnostic.
#
# Chapter 02 checks:
#   * a .bss array (p_memsz > p_filesz) reads back as zero;
#   * sp is 16-byte aligned, inside the stack region, and its page is mapped;
#   * an access outside mapped memory is a trap (exit code 126).
#
# Chapter 03 checks:
#   * --selftest passes every hand-computed instruction vector;
#   * all 97 corpus instructions decode to exactly llvm-mc's text;
#   * the five programs from chapter 01 run to the first ecall with
#     main() returning 0 in t0.
#
# Chapter 04 checks:
#   * tests/control-flow.s exercises both branch directions, jal linking and
#     a jalr whose target has bit 0 set; main() must return 0;
#   * a misaligned jump/load traps with the *faulting* pc as epc, checked
#     against llvm-objdump's address of that instruction.
#
# Chapter 05 checks:
#   * the five programs produce byte-identical stdout and exit codes under
#     rsim and qemu-riscv32-static;
#   * exit(2) propagates the guest's status (42);
#   * an unknown syscall stops with status 127.
#
# Chapter 06 checks:
#   * --disas reproduces llvm-mc's text for .text exactly, and covers exactly
#     the address/word pairs llvm-objdump -d lists (objdump prints aliases and
#     hex immediates, so its text is not comparable directly);
#   * --trace visits only those addresses, its line count equals the executed
#     instruction count, and each trace line matches --disas at that address.
#
# Chapter 07 checks:
#   * a scripted session's transcript is byte-identical to the golden file;
#   * b resolves through .symtab, a breakpoint on the current pc stops before
#     the instruction runs, and `s 2` advances exactly two instructions.
#
# Chapter 08 checks:
#   * an unknown encoding traps with mcause=2 and mepc == the address of main;
#   * ebreak traps with mcause=3 and mtval == mepc;
#   * tests/trap-handler.s installs mtvec, validates mcause/mepc/mtval for
#     both traps, skips the faulting instruction and returns with mret.
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

# --- Chapter 02: .bss, stack, traps -------------------------------------

cat > "$TMP/bss.c" <<'C'
int zeroed[16];
int initialized = 0x12345678;
int main(void) { return zeroed[0] + initialized; }
C
build_program bss "$TMP/bss.c" 2

# The zeroed array lives past p_filesz, in the tail of a PT_LOAD that has
# p_memsz > p_filesz.  If the loader forgot to zero-fill, the dump below
# would show stale bytes.
python3 - "$READELF" "$TMP/bss" <<'PY'
import re, subprocess, sys
readelf, elf = sys.argv[1:3]
out = subprocess.run([readelf, "-l", elf], capture_output=True, text=True,
                     check=True).stdout
ok = False
for line in out.splitlines():
    m = re.search(r"LOAD\s+0x[0-9a-f]+\s+0x[0-9a-f]+\s+0x[0-9a-f]+\s+"
                  r"0x([0-9a-f]+)\s+0x([0-9a-f]+)", line)
    if m and int(m.group(2), 16) > int(m.group(1), 16):
        ok = True
if not ok:
    sys.exit("no PT_LOAD with p_memsz > p_filesz; the .bss case is not set up")
PY
CHECKS=$((CHECKS + 1))

ADDR=$("$READELF" -s "$TMP/bss" | awk '$8 == "zeroed" {print "0x" $2; exit}')
[[ -n "$ADDR" ]] || fail "zeroed symbol not found in .symtab"
"$RSIM" --dump-mem "$TMP/bss" "$ADDR" 64 > "$TMP/bss.dump" \
  || fail "rsim could not dump the .bss array"
[[ -z "$(tr -d ' 0\n' < "$TMP/bss.dump")" ]] \
  || fail ".bss array is not zero: $(cat "$TMP/bss.dump")"
CHECKS=$((CHECKS + 1))

# sp must be 16-byte aligned and inside the documented stack region, and the
# stack must be mapped (dump the bytes just below sp).
"$RSIM" "$TMP/demo" > "$TMP/state.txt" || fail "rsim could not reset demo"
SP=$(sed -n 's/.*sp 0x\([0-9a-f]*\).*/\1/p' "$TMP/state.txt")
[[ -n "$SP" ]] || fail "reset output has no sp"
python3 - "$SP" <<'PY'
import sys
sp = int(sys.argv[1], 16)
assert sp % 16 == 0, "sp 0x%08x is not 16-byte aligned" % sp
assert 0x7ff00000 <= sp < 0x80000000, "sp 0x%08x is outside the stack region" % sp
PY
"$RSIM" --dump-mem "$TMP/demo" "$((16#$SP - 16))" 16 > /dev/null \
  || fail "the stack page below sp is not mapped"
CHECKS=$((CHECKS + 1))

# An access to unmapped memory is a trap, not a silent zero.
set +e
"$RSIM" --dump-mem "$TMP/bss" 0x90000000 4 > "$TMP/oob.out" 2> "$TMP/oob.err"
OOB_RC=$?
set -e
[[ "$OOB_RC" -eq 126 ]] || fail "out-of-bounds access exited $OOB_RC, want 126"
grep -q '^rsim: trap: ' "$TMP/oob.err" || fail "out-of-bounds access had no trap line"
CHECKS=$((CHECKS + 1))

# --- Chapter 03: decode and execute ------------------------------------

"$RSIM" --selftest > "$TMP/selftest.txt" || fail "selftest reported failures"
grep -q '0 failures' "$TMP/selftest.txt" || fail "selftest summary is not clean"
CHECKS=$((CHECKS + 1))

# llvm-mc -disassemble reads hex text, so feed it and rsim the same bytes.
hex_words() {
  xxd -p -c4 "$1" | awk '{printf "0x%s 0x%s 0x%s 0x%s\n",
                          substr($0,1,2), substr($0,3,2),
                          substr($0,5,2), substr($0,7,2)}'
}

decode_corpus() {
  local name=$1 mattr=${2:-}
  local mc_args=(-triple=rvmin)
  [[ -n "$mattr" ]] && mc_args+=(-mattr="$mattr")

  "$MC" "${mc_args[@]}" -filetype=obj "$CORPUS/$name.s" -o "$TMP/$name.o" \
    || fail "llvm-mc rejected $name.s"
  "$BIN/llvm-objcopy" -O binary --only-section=.text "$TMP/$name.o" "$TMP/$name.bin" \
    || fail "llvm-objcopy could not extract .text from $name.o"
  hex_words "$TMP/$name.bin" > "$TMP/$name.hex"
  "$RSIM" --decode-hex "$TMP/$name.hex" > "$TMP/$name.ours" \
    || fail "rsim could not decode $name"
  "$MC" "${mc_args[@]}" -disassemble "$TMP/$name.hex" \
    | sed 's/^\t//' > "$TMP/$name.ref" || fail "llvm-mc could not disassemble $name"
  diff -u "$TMP/$name.ref" "$TMP/$name.ours" > "$TMP/$name.diff" \
    || fail "$name: decode differs from llvm-mc (see $TMP/$name.diff)"
  CHECKS=$((CHECKS + $(wc -l < "$TMP/$name.ours")))
}

CORPUS="$COURSE2/../llvm/lib/Target/RVMin/scripts"
decode_corpus rvmin-encoding-corpus
decode_corpus rvmin-m-encoding-corpus +m

for name in demo muldiv_m large_frame switch tail; do
  max_steps=10000000
  [[ "$name" == tail ]] && max_steps=50000000
  "$RSIM" --run "$TMP/$name" --max-steps "$max_steps" > "$TMP/$name.run" 2>&1 \
    || fail "$name did not halt under rsim"
  grep -q 't0=0x00000000' "$TMP/$name.run" \
    || fail "$name: main() did not return 0 in t0 ($(cat "$TMP/$name.run"))"
  CHECKS=$((CHECKS + 1))
done

# --- Chapter 04: control flow and precise traps -------------------------

build_test_asm() {
  local name=$1 src=$2
  "$MC" -triple=rvmin -filetype=obj "$src" -o "$TMP/$name.o" \
    || fail "llvm-mc rejected $src"
  "$MC" -triple=rvmin -filetype=obj "$RT/start.S" -o "$TMP/start-$name.o" \
    || fail "llvm-mc rejected start.S for $name"
  "$MC" -triple=rvmin -filetype=obj "$RT/rvmin-rt.S" -o "$TMP/rt-$name.o" \
    || fail "llvm-mc rejected rvmin-rt.S for $name"
  "$LD" -m elf32lriscv -T "$RT/link.ld" -o "$TMP/$name" \
    "$TMP/start-$name.o" "$TMP/$name.o" "$TMP/rt-$name.o" \
    || fail "ld.lld could not link $name"
}

build_test_asm control_flow "$COURSE2/tests/control-flow.s"
"$RSIM" --run "$TMP/control_flow" > "$TMP/control_flow.run" 2>&1 \
  || fail "control-flow.s trapped or hit the step limit"
grep -q 't0=0x00000000' "$TMP/control_flow.run" \
  || fail "control-flow.s failed ($(cat "$TMP/control_flow.run"))"
CHECKS=$((CHECKS + 1))

# The trapped pc must be the faulting instruction, not the one after it.
check_precise_trap() {
  local name=$1 file=$2 cause=$3 tval=$4
  set +e
  "$RSIM" --run "$TMP/$file" --max-steps 100 > "$TMP/$file.out" 2> "$TMP/$file.err"
  local rc=$?
  set -e
  [[ "$rc" -eq 126 ]] || fail "$file: exit code $rc, want 126"
  grep -q "^rsim: trap: $cause: tval=$tval " "$TMP/$file.err" \
    || fail "$file: wrong trap line: $(cat "$TMP/$file.err")"
  local epc line
  epc=$(sed -n 's/.*epc=0x\([0-9a-f]*\).*/\1/p' "$TMP/$file.err")
  line=$("$BIN/llvm-objdump" -d "$TMP/$file" \
         | awk -v a="$(printf '%x' $((16#$epc)))" \
               '$1 == a":" { found = $0 } END { print found }')
  [[ -n "$line" ]] || fail "$file: objdump has no word at epc 0x$epc"
  grep -q "\<$name\>" <<<"$line" \
    || fail "$file: epc 0x$epc is not a $name ($line)"
  CHECKS=$((CHECKS + 1))
}

build_test_asm misaligned_jump "$COURSE2/tests/misaligned-jump.s"
build_test_asm misaligned_load "$COURSE2/tests/misaligned-load.s"
check_precise_trap jalr misaligned_jump "instruction address misaligned" 0x20000002
check_precise_trap lw misaligned_load "load address misaligned" 0x20000002

# --- Chapter 05: system calls and the runtime ---------------------------

QEMU=${QEMU:-qemu-riscv32-static}
command -v "$QEMU" > /dev/null \
  || fail "chapter 05 needs QEMU ($QEMU) for the differential test"

for name in demo muldiv_m large_frame switch tail; do
  set +e
  "$RSIM" --run "$TMP/$name" --max-steps 50000000 \
    > "$TMP/$name.rsim.out" 2> "$TMP/$name.rsim.err"
  rsim_rc=$?
  "$QEMU" "$TMP/$name" > "$TMP/$name.qemu.out" 2> /dev/null
  qemu_rc=$?
  set -e

  cmp -s "$TMP/$name.rsim.out" "$TMP/$name.qemu.out" \
    || fail "$name: stdout differs from QEMU ($(cat "$TMP/$name.rsim.out"))"
  [[ "$rsim_rc" -eq "$qemu_rc" ]] \
    || fail "$name: exit code $rsim_rc, QEMU says $qemu_rc"
  CHECKS=$((CHECKS + 1))
done

# The five programs are the runtime's own self-check: they print this on success.
grep -q '^rvmin: ok$' "$TMP/demo.rsim.out" || fail "demo did not print rvmin: ok"

build_test_asm exit_code "$COURSE2/tests/exit-code.s"
set +e
"$RSIM" --run "$TMP/exit_code" > /dev/null 2>&1
rsim_rc=$?
"$QEMU" "$TMP/exit_code" > /dev/null 2>&1
qemu_rc=$?
set -e
[[ "$rsim_rc" -eq 42 ]] || fail "exit-code.s: rsim exited $rsim_rc, want 42"
[[ "$qemu_rc" -eq 42 ]] || fail "exit-code.s: QEMU exited $qemu_rc, want 42"
CHECKS=$((CHECKS + 1))

build_test_asm unknown_syscall "$COURSE2/tests/unknown-syscall.s"
set +e
"$RSIM" --run "$TMP/unknown_syscall" > /dev/null 2> "$TMP/unknown.err"
rsim_rc=$?
set -e
[[ "$rsim_rc" -eq 127 ]] || fail "unknown syscall exited $rsim_rc, want 127"
grep -q 'unknown ecall a7=999' "$TMP/unknown.err" \
  || fail "unknown syscall left no diagnostic"
CHECKS=$((CHECKS + 1))

# --- Chapter 06: trace and self-disassembly ----------------------------

"$RSIM" --disas "$TMP/demo" > "$TMP/demo.disas"
"$BIN/llvm-objcopy" -O binary --only-section=.text "$TMP/demo" "$TMP/demo.text"
hex_words "$TMP/demo.text" > "$TMP/demo.text.hex"
"$MC" -triple=rvmin -disassemble "$TMP/demo.text.hex" \
  | sed 's/^\t//' > "$TMP/demo.ref"
sed 's/^[0-9a-f]* [0-9a-f]* //' "$TMP/demo.disas" > "$TMP/demo.ours"
diff -u "$TMP/demo.ref" "$TMP/demo.ours" > "$TMP/demo.diff" \
  || fail "--disas text differs from llvm-mc (see $TMP/demo.diff)"
CHECKS=$((CHECKS + 1))

# llvm-objdump prints aliases (mv/li/j/ret) and hex immediates, so compare the
# address/word pairs instead of the text; the text is already covered above.
"$BIN/llvm-objdump" -d "$TMP/demo" > "$TMP/demo.objdump"
python3 - "$TMP/demo.disas" "$TMP/demo.objdump" <<'PY'
import re
import sys

disas, objdump = sys.argv[1:3]

ref = {}
for line in open(objdump):
    m = re.match(r"\s*([0-9a-f]+):\s+([0-9a-f]+)\s", line)
    if m:
        ref[int(m.group(1), 16)] = int(m.group(2), 16)

ours = {}
for line in open(disas):
    addr, word, _ = line.split(" ", 2)
    ours[int(addr, 16)] = int(word, 16)

if ours != ref:
    only_ours = sorted(set(ours) - set(ref))
    only_ref = sorted(set(ref) - set(ours))
    sys.exit("--disas and llvm-objdump disagree: %d only in rsim %s, "
             "%d only in objdump %s"
             % (len(only_ours), [hex(a) for a in only_ours[:4]],
                len(only_ref), [hex(a) for a in only_ref[:4]]))
PY
CHECKS=$((CHECKS + 1))

set +e
"$RSIM" --run "$TMP/demo" --trace --max-steps 10000000 \
  > /dev/null 2> "$TMP/demo.trace"
trace_rc=$?
set -e
[[ "$trace_rc" -eq 0 ]] || fail "trace run of demo exited $trace_rc"

python3 - "$TMP/demo.trace" "$TMP/demo.disas" <<'PY'
import re
import sys

trace, disas = sys.argv[1:3]

text = {}
for line in open(disas):
    addr, _, rest = line.split(" ", 2)
    text[int(addr, 16)] = rest.strip()

visited = []
wrong = None
for line in open(trace):
    if not line.startswith("T "):
        continue
    _, addr, word, rest = line.rstrip("\n").split(" ", 3)
    at = int(addr, 16)
    visited.append(at)
    if at not in text:
        wrong = "trace visited 0x%08x, which --disas does not cover" % at
        break
    if text[at] != rest:
        wrong = "trace text at 0x%08x differs from --disas" % at
        break
if wrong:
    sys.exit(wrong)

if not visited:
    sys.exit("trace is empty")

summary = [l for l in open(trace) if "instructions (" in l]
m = re.search(r"after (\d+) instructions", summary[-1] if summary else "")
if not m:
    sys.exit("no instruction count in the halt summary")
if int(m.group(1)) != len(visited):
    sys.exit("trace has %d lines, the CPU executed %s instructions"
             % (len(visited), m.group(1)))
PY
CHECKS=$((CHECKS + 1))

# --- Chapter 07: breakpoints, stepping and scripted debugging -----------

"$RSIM" --debug "$TMP/demo" --script "$COURSE2/tests/debug-session.txt" \
  > "$TMP/debug.transcript" 2> "$TMP/debug.err" \
  || fail "debug session failed: $(cat "$TMP/debug.err")"
cmp -s "$TMP/debug.transcript" "$COURSE2/tests/debug-session.golden" \
  || fail "debug transcript differs from tests/debug-session.golden"
CHECKS=$((CHECKS + 1))

# A breakpoint set on the current pc must fire before the instruction runs;
# resuming from a reported stop must step over it exactly once.
printf 'b _start\nc\ns 2\nq\n' > "$TMP/debug2.txt"
"$RSIM" --debug "$TMP/demo" --script "$TMP/debug2.txt" \
  > "$TMP/debug2.out" 2> /dev/null || fail "second debug session failed"
grep -q '^breakpoint 0: 0x00010000 <_start>$' "$TMP/debug2.out" \
  || fail "b _start did not resolve through .symtab"
grep -q '^stopped at 0x00010000 <_start>$' "$TMP/debug2.out" \
  || fail "c did not stop on a breakpoint set at the current pc"
grep -q '^stopped at 0x00010008 <_start+8>$' "$TMP/debug2.out" \
  || fail "s 2 did not advance exactly two instructions"
CHECKS=$((CHECKS + 1))

# --- Chapter 08: the trap model ----------------------------------------

# The course-1 assembler has no Zicsr, so these three use clang's RISC-V
# assembler.  The encodings are standard and chapter 03 already proved the
# decoder agrees with the reference on every RV32I word.
build_csr_program() {
  local name=$1 src=$2
  local cflags=(--target="$TRIPLE" -march=rv32i_zicsr -mabi=ilp32 -mno-relax -c)
  "$CC" "${cflags[@]}" "$src" -o "$TMP/$name.o" \
    || fail "clang could not assemble $src"
  "$CC" "${cflags[@]}" "$RT/start.S" -o "$TMP/start-$name.o" \
    || fail "clang could not assemble start.S for $name"
  "$CC" "${cflags[@]}" "$RT/rvmin-rt.S" -o "$TMP/rt-$name.o" \
    || fail "clang could not assemble rvmin-rt.S for $name"
  "$LD" -m elf32lriscv -T "$RT/link.ld" -o "$TMP/$name" \
    "$TMP/start-$name.o" "$TMP/$name.o" "$TMP/rt-$name.o" \
    || fail "ld.lld could not link $name"
}

csr_field() {
  sed -n "s/^$1 *0x\\([0-9a-f]*\\)$/\\1/p" "$TMP/$2.crs"
}

build_csr_program illegal "$COURSE2/tests/illegal.s"
set +e
"$RSIM" --run "$TMP/illegal" --dump-csr > "$TMP/illegal.crs" 2> "$TMP/illegal.err"
illegal_rc=$?
set -e
[[ "$illegal_rc" -eq 126 ]] || fail "illegal.s exited $illegal_rc, want 126"
grep -q '^mcause   0x00000002$' "$TMP/illegal.crs" \
  || fail "illegal instruction did not set mcause=2"
grep -q 'illegal instruction' "$TMP/illegal.err" \
  || fail "illegal instruction left no diagnostic"
MAIN=$("$READELF" -s "$TMP/illegal" | awk '$8 == "main" { print "0x" $2; exit }')
[[ -n "$MAIN" ]] || fail "illegal: no main symbol"
[[ $((16#$(csr_field mepc illegal))) -eq $((MAIN)) ]] \
  || fail "illegal: mepc is not main's address"
CHECKS=$((CHECKS + 1))

build_csr_program ebreak "$COURSE2/tests/ebreak.s"
set +e
"$RSIM" --run "$TMP/ebreak" --dump-csr > "$TMP/ebreak.crs" 2> /dev/null
ebreak_rc=$?
set -e
[[ "$ebreak_rc" -eq 126 ]] || fail "ebreak.s exited $ebreak_rc, want 126"
grep -q '^mcause   0x00000003$' "$TMP/ebreak.crs" \
  || fail "ebreak did not set mcause=3"
[[ "$(csr_field mepc ebreak)" == "$(csr_field mtval ebreak)" ]] \
  || fail "ebreak: mtval should be the ebreak address"
CHECKS=$((CHECKS + 1))

build_csr_program trap_handler "$COURSE2/tests/trap-handler.s"
"$RSIM" --run "$TMP/trap_handler" --dump-csr > "$TMP/trap_handler.crs" \
  2> "$TMP/trap_handler.err" \
  || fail "trap-handler.s failed: $(cat "$TMP/trap_handler.err")"
grep -q '^mcause   0x00000003$' "$TMP/trap_handler.crs" \
  || fail "trap-handler did not take both traps"
CHECKS=$((CHECKS + 1))

echo "$CHECKS checks"
