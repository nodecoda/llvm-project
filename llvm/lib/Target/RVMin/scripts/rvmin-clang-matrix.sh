#!/usr/bin/env bash
#
# Chapter 28 gate: the clang driver matrix.
#
# Usage: rvmin-clang-matrix.sh <llvm-bin-dir>
#
# It runs the whole e2e corpus through `clang --target=rvmin` in every cell of
#
#     {rv32i, rv32im, rv32if, rv32id, rv32ic, rv32imafdc,
#      rv32g, rv32gc}
#         x {-O0, -O1, -O2, -O3} x scripts/e2e/*.c
#
# (12 programs, 384 objects) and checks three independent things per cell:
#
#   1. Driver: `clang --target=rvmin -c` exits 0, prints nothing on stderr and
#      produces a 32-bit RISC-V ELF object.
#   2. Assembler: the object from step 1 has exactly the same .text as the
#      object built from the compiler's own assembly output, so `-S` and `-c`
#      agree and the integrated assembler is faithful.
#   3. TargetInfo parity: the IR clang emits for rvmin and for riscv32 (same
#      ISA, same flags) lowers to byte-identical .text when both are fed to
#      the *same* rvmin backend with the same features.  This is the check
#      that proves rvmin's TargetInfo/driver do not perturb code generation.
#
# Two deliberate details in check 3:
#
#   * `llc` builds one subtarget from its command line and never reads the
#     "target-features" attributes in the IR, while the clang driver passes
#     the features to the TargetMachine.  The baseline therefore has to spell
#     the feature string out with -mattr; otherwise it would silently compare
#     an RV32I build against an RV32IM build.
#   * The vectorizers are disabled on both sides.  The two IRs carry
#     different target-features strings and rvmin's backend ignores them, but
#     the IR pipeline runs *before* the backend, and the SLP cost model
#     consults the TargetMachine: RVMin's TTI and RISC-V's TTI legitimately
#     pick different reassociation trees for a long reduction.  That is a
#     vectorizer policy difference, not an ABI difference; with the
#     vectorizers off the comparison isolates exactly the ABI/attribute
#     surface this gate is about.
#
set -euo pipefail

BIN=${1:?usage: rvmin-clang-matrix.sh <llvm-bin-dir>}
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
E2E="$HERE/e2e"
CC="$BIN/clang"
LLC="$BIN/llc"
READELF="$BIN/llvm-readelf"
OBJCOPY="$BIN/llvm-objcopy"
OBJDUMP="$BIN/llvm-objdump"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

dump_text() {
  "$OBJCOPY" --dump-section ".text=$2" "$1" 2> /dev/null \
    || fail "no .text section in $1"
}

# The same flags on both sides of the TargetInfo comparison: no vectorizer.
PARITY_FLAGS=(-ffreestanding -fno-builtin -fno-slp-vectorize -fno-vectorize)

OBJECTS=0
for march in rv32i rv32im rv32if rv32id rv32ic rv32imafdc rv32g rv32gc; do
  case "$march" in
    rv32i)  MATTR="";   MABI=ilp32 ;;
    rv32im) MATTR="+m"; MABI=ilp32 ;;
    # Chapter 40: the driver derives ilp32f from rv32if; spelling it out keeps
    # the intent obvious and matches what the backend expects.
    rv32if) MATTR="+f";    MABI=ilp32f ;;
    # Chapter 41: D implies F; the driver derives ilp32d from rv32id.
    rv32id) MATTR="+f,+d"; MABI=ilp32d ;;
    # Chapter 44: C does not change the ABI; the full string needs ilp32d.
    rv32ic) MATTR="+c"; MABI=ilp32 ;;
    rv32imafdc) MATTR="+m,+f,+d,+c"; MABI=ilp32d ;;
    # Chapter 46: g is i/m/a/f/d/zicsr/zifencei; gc adds C.  Both are now
    # spelled entirely by features the backend implements, so the chapter 43
    # driver rejection is gone.
    rv32g)  MATTR="+m,+a,+f,+d,+zicsr,+zifencei"; MABI=ilp32d ;;
    rv32gc) MATTR="+m,+a,+f,+d,+c,+zicsr,+zifencei"; MABI=ilp32d ;;
    *) fail "unexpected -march=$march" ;;
  esac

  # The M extension must be visible in the object, and absent without it.
  "$CC" --target=rvmin-unknown-elf -march="$march" -mabi="$MABI" -O2 \
    -ffreestanding -fno-builtin -c "$E2E/arith.c" -o "$TMP/isa-$march.o"
  # RVMin emits no .riscv.attributes, so the disassembler has to be told about
  # the extensions it should decode.
  "$OBJDUMP" -d --mattr=+m,+f,+d,+c "$TMP/isa-$march.o" > "$TMP/isa-$march.dis"
  if grep -Eq '^[[:space:]]+[0-9a-f]+:[[:space:]]+[0-9a-f]+[[:space:]]+mul[[:space:]]' \
       "$TMP/isa-$march.dis"; then
    [[ "$march" == rv32im || "$march" == rv32imafdc || "$march" == rv32g ||
       "$march" == rv32gc ]] \
      || fail "$march object contains a mul instruction"
  else
    [[ "$march" != rv32im && "$march" != rv32imafdc && "$march" != rv32g &&
       "$march" != rv32gc ]] \
      || fail "$march object does not contain a mul instruction"
  fi

  # Chapter 44: C must be visible exactly where the ISA string says so.
  # Test the raw-byte column: a 16-bit instruction prints as four hex digits
  # ("c606"), a 32-bit one as eight ("000002b7").  Mnemonics are not usable
  # here because llvm-objdump prints aliases (`sw`, `mv`, `jalr`).
  if grep -Eq '^[[:space:]]+[0-9a-f]+:[[:space:]]+[0-9a-f]{4}[[:space:]]' \
       "$TMP/isa-$march.dis"; then
    [[ "$march" == rv32ic || "$march" == rv32imafdc || "$march" == rv32gc ]] \
      || fail "$march object contains a compressed instruction"
  else
    [[ "$march" != rv32ic && "$march" != rv32imafdc && "$march" != rv32gc ]] \
      || fail "$march object lacks compressed instructions"
  fi

  # The FP features must be visible exactly where the ISA string says so.
  "$CC" --target=rvmin-unknown-elf -march="$march" -mabi="$MABI" -O2 \
    -ffreestanding -fno-builtin -c "$E2E/fp.c" -o "$TMP/fp-$march.o"
  "$OBJDUMP" -d --mattr=+f,+d "$TMP/fp-$march.o" > "$TMP/fp-$march.dis"
  if grep -Eq '^[[:space:]]+[0-9a-f]+:[[:space:]]+[0-9a-f]+[[:space:]]+fadd\.s' \
       "$TMP/fp-$march.dis"; then
    [[ "$march" == rv32if || "$march" == rv32id || "$march" == rv32imafdc ||
       "$march" == rv32g || "$march" == rv32gc ]] \
      || fail "$march fp.c object contains fadd.s"
  else
    [[ "$march" == rv32i || "$march" == rv32im || "$march" == rv32ic ]] \
      || fail "$march fp.c object lacks fadd.s"
  fi

  "$CC" --target=rvmin-unknown-elf -march="$march" -mabi="$MABI" -O2 \
    -ffreestanding -fno-builtin -c "$E2E/fp-double.c" -o "$TMP/fpd-$march.o"
  "$OBJDUMP" -d --mattr=+f,+d "$TMP/fpd-$march.o" > "$TMP/fpd-$march.dis"
  if grep -Eq '^[[:space:]]+[0-9a-f]+:[[:space:]]+[0-9a-f]+[[:space:]]+fadd\.d' \
       "$TMP/fpd-$march.dis"; then
    [[ "$march" == rv32id || "$march" == rv32imafdc || "$march" == rv32g ||
       "$march" == rv32gc ]] \
      || fail "$march fp-double.c object contains fadd.d"
  else
    [[ "$march" != rv32id && "$march" != rv32imafdc && "$march" != rv32g &&
       "$march" != rv32gc ]] \
      || fail "$march fp-double.c object lacks fadd.d"
  fi

  for opt in O0 O1 O2 O3; do
    for src in "$E2E"/*.c; do
      name=$(basename "$src" .c)
      stem="$TMP/$name-$march-$opt"

      # 1. The driver path: one clang invocation, straight to an object.
      "$CC" --target=rvmin-unknown-elf -march="$march" -mabi="$MABI" \
        -"$opt" -ffreestanding -fno-builtin -c "$src" -o "$stem.o" \
        2> "$stem.log" \
        || { cat "$stem.log" >&2; fail "clang -c failed for $name ($march, -$opt)"; }
      if [[ -s "$stem.log" ]]; then
        cat "$stem.log" >&2
        fail "clang printed diagnostics for $name ($march, -$opt)"
      fi
      "$READELF" -h "$stem.o" | grep -q 'Class:.*ELF32' \
        || fail "$name ($march, -$opt) is not ELF32"
      "$READELF" -h "$stem.o" | grep -q 'Machine:.*RISC-V' \
        || fail "$name ($march, -$opt) is not EM_RISCV"

      # 2. -S then assemble must reproduce the -c object.
      "$CC" --target=rvmin-unknown-elf -march="$march" -mabi="$MABI" \
        -"$opt" -ffreestanding -fno-builtin -S "$src" -o "$stem.s" \
        || fail "clang -S failed for $name ($march, -$opt)"
      # -O is meaningless for an assembler input and clang warns about it.
      "$CC" --target=rvmin-unknown-elf -march="$march" -mabi="$MABI" \
        -c "$stem.s" -o "$stem.asm.o" \
        || fail "integrated assembler failed for $name ($march, -$opt)"
      dump_text "$stem.o" "$stem.text"
      dump_text "$stem.asm.o" "$stem.asm.text"
      cmp -s "$stem.text" "$stem.asm.text" \
        || fail "-S and -c disagree for $name ($march, -$opt)"

      # 3. TargetInfo parity, both sides through the same rvmin backend.
      "$CC" --target=rvmin-unknown-elf -march="$march" -mabi="$MABI" \
        -"$opt" "${PARITY_FLAGS[@]}" -S -emit-llvm "$src" -o "$stem.rvmin.ll" \
        || fail "rvmin -emit-llvm failed for $name ($march, -$opt)"
      "$CC" --target=riscv32-unknown-elf -march="$march" -mabi="$MABI" \
        -"$opt" "${PARITY_FLAGS[@]}" -S -emit-llvm "$src" -o "$stem.riscv.ll" \
        || fail "riscv32 -emit-llvm failed for $name ($march, -$opt)"
      LLC_ARGS=(-march=rvmin -mtriple=rvmin-unknown-elf -"$opt" -filetype=obj)
      [[ -n "$MATTR" ]] && LLC_ARGS+=(-mattr="$MATTR")
      "$LLC" "${LLC_ARGS[@]}" "$stem.rvmin.ll" -o "$stem.rvmin.o" \
        || fail "llc failed for rvmin IR ($name, $march, -$opt)"
      "$LLC" "${LLC_ARGS[@]}" "$stem.riscv.ll" -o "$stem.riscv.o" \
        || fail "llc failed for riscv32 IR ($name, $march, -$opt)"
      dump_text "$stem.rvmin.o" "$stem.rvmin.text"
      dump_text "$stem.riscv.o" "$stem.riscv.text"
      if ! cmp -s "$stem.rvmin.text" "$stem.riscv.text"; then
        echo "--- first differing .text bytes ($name, $march, -$opt) ---" >&2
        cmp -l "$stem.rvmin.text" "$stem.riscv.text" | head -10 >&2 || true
        fail "TargetInfo changed code generation for $name ($march, -$opt)"
      fi

      OBJECTS=$((OBJECTS + 1))
    done
    echo "PASS: e2e corpus under -march=$march -$opt"
  done
done

echo "ALL CLANG MATRIX PASS: $OBJECTS objects, ELF32 EM_RISCV, -S/-c agree, .text matches the riscv32 TargetInfo baseline"
