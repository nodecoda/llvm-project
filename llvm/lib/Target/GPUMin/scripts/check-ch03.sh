#!/usr/bin/env bash
#
# Chapter 03 gate: a real function, a real object file, a real disassembly.
#
# Usage: check-ch03.sh <llvm-bin-dir>
#
# What each group proves:
#   1. `llc` lowers a function with arguments, a returned constant and a
#      returned argument -- and the return value actually reaches r4 (the
#      copy is live, see "liveness" below);
#   2. the object file declares EM_GPUMIN and the reader maps it back to
#      `gpumin` instead of guessing RISC-V;
#   3. `llvm-objdump -d` decodes the .o with the GPUMin disassembler;
#   4. the .o produced by `llc` and the one produced by assembling the .s are
#      byte-identical, so the codegen path and the MC path agree.
#
# The liveness check is the point of (1).  Without register operands on the
# RETURN DAG node, dead-machine-instruction elimination deletes
# `$r4 = COPY %v` and the function silently returns whatever r4 held.
#
set -euo pipefail

BIN=${1:?usage: check-ch03.sh <llvm-bin-dir>}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

cat > "$TMP/ch03.ll" <<'IR'
target triple = "gpumin-unknown-elf"

define i32 @pick_second(i32 %a, i32 %b) {
  ret i32 %b
}

define i32 @forty_two() {
  ret i32 42
}

define void @k() #0 {
  ret void
}

attributes #0 = { "gpumin_kernel" }
IR

# --- 1. instruction selection + return-value liveness -----------------------
"$BIN/llc" -march=gpumin -verify-machineinstrs "$TMP/ch03.ll" -o "$TMP/ch03.s" \
  || fail "llc rejected the module"

grep -q 'mov r4, r5' "$TMP/ch03.s" \
  || fail "the returned argument never reached r4 (copy eliminated as dead)"
grep -q 'movi r4, 42' "$TMP/ch03.s" \
  || fail "the returned constant never reached r4"
[ "$(grep -c '^	ret$' "$TMP/ch03.s")" -eq 2 ] \
  || fail "each device function must end with exactly one ret"
[ "$(grep -c '^	exit$' "$TMP/ch03.s")" -eq 1 ] \
  || fail "the kernel must end with exactly one exit"
echo "PASS 1/4: arguments, returned constants and returned values lower correctly"

# --- 2. ELF machine number --------------------------------------------------
"$BIN/llc" -march=gpumin -filetype=obj "$TMP/ch03.ll" -o "$TMP/ch03.o" \
  || fail "llc could not write an object file"

"$BIN/llvm-readobj" -h "$TMP/ch03.o" > "$TMP/hdr.txt"
grep -q 'Format: elf32-gpumin' "$TMP/hdr.txt" \
  || fail "the object file does not declare the gpumin format"
grep -q 'Arch: gpumin' "$TMP/hdr.txt" \
  || fail "getArch() did not map EM_GPUMIN back to Triple::gpumin"
grep -q 'Machine:.*EM_GPUMIN' "$TMP/hdr.txt" \
  || fail "the ELF header does not carry EM_GPUMIN"
echo "PASS 2/4: the object file is elf32-gpumin and reads back as gpumin"

# --- 3. objdump -------------------------------------------------------------
"$BIN/llvm-objdump" -d "$TMP/ch03.o" > "$TMP/objdump.txt"
grep -q $'\tmov r4, r5$' "$TMP/objdump.txt" \
  || fail "objdump did not decode the mov"
grep -q $'\tmovi r4, 42$' "$TMP/objdump.txt" \
  || fail "objdump did not decode the movi"
[ "$(grep -c $'\tret$' "$TMP/objdump.txt")" -eq 2 ] \
  || fail "objdump did not decode both device returns"
[ "$(grep -c $'\texit$' "$TMP/objdump.txt")" -eq 1 ] \
  || fail "objdump did not decode the kernel exit"
echo "PASS 3/4: llvm-objdump -d decodes the object file"

# --- 4. codegen path == MC path --------------------------------------------
"$BIN/llvm-mc" -triple=gpumin -filetype=obj "$TMP/ch03.s" -o "$TMP/ch03_mc.o" \
  || fail "assembling the .s failed"
"$BIN/llvm-objdump" -d "$TMP/ch03.o"    | grep -v "file format" > "$TMP/a.txt"
"$BIN/llvm-objdump" -d "$TMP/ch03_mc.o" | grep -v "file format" > "$TMP/b.txt"
diff -u "$TMP/a.txt" "$TMP/b.txt" > "$TMP/c.txt" || {
  cat "$TMP/c.txt" >&2
  fail "the object file and the assembler disagree on the encoding"
}
echo "PASS 4/4: llc's .o and the reassembled .s are identical"

echo "OK: chapter 03 gate passed"
