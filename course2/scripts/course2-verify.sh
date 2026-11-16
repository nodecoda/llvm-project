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
  "$RSIM" --run "$TMP/$name" --max-steps "$max_steps" > "$TMP/$name.run" \
    || fail "$name did not halt under rsim"
  grep -q 't0=0x00000000' "$TMP/$name.run" \
    || fail "$name: main() did not return 0 in t0 ($(cat "$TMP/$name.run"))"
  CHECKS=$((CHECKS + 1))
done

echo "$CHECKS checks"
