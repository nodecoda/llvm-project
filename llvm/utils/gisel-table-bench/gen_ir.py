#!/usr/bin/env python3
"""Generate the x86 legalizer-table benchmark corpus.

The corpus is plain LLVM IR for x86_64, restricted to functions the X86
GlobalISel legalizer can fully legalize.  That restriction is load-bearing:
``legalizeMachineFunction`` returns on the *first* instruction it cannot
legalize and drops the rest of the function, so a single unsupported shape
would silently shorten the measured work.  ``bench.py`` re-checks it.

Coverage follows the rule sets whose chains are longest, because that is where
one table lookup has the most to replace: scalar integer arithmetic and
comparisons at every width from i1 to i256, div/rem, bit manipulation,
int<->float conversion, vectors, and memory of every scalar and vector size.
"""

import argparse
import os
import sys

DECLS = """\
declare i16 @llvm.bswap.i16(i16)
declare i32 @llvm.bswap.i32(i32)
declare i64 @llvm.bswap.i64(i64)
declare i32 @llvm.smax.i32(i32, i32)
declare i32 @llvm.smin.i32(i32, i32)
declare i32 @llvm.umax.i32(i32, i32)
declare i32 @llvm.umin.i32(i32, i32)
declare i64 @llvm.smax.i64(i64, i64)
declare i64 @llvm.smin.i64(i64, i64)
declare i64 @llvm.umax.i64(i64, i64)
declare i64 @llvm.umin.i64(i64, i64)
declare { i64, i1 } @llvm.uadd.with.overflow.i64(i64, i64)
declare float @llvm.sqrt.f32(float)
declare double @llvm.sqrt.f64(double)
declare float @llvm.fabs.f32(float)
declare double @llvm.fabs.f64(double)
declare float @llvm.fma.f32(float, float, float)
declare double @llvm.fma.f64(double, double, double)
declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1)
declare void @llvm.memset.p0.i64(ptr, i8, i64, i1)
"""


def _n(ty):
    """A name fragment for a type: i64 -> i64, <4 x i32> -> v4xi32."""
    return ty.replace("<", "v").replace(" x", "x").replace(">", "").replace(" ", "")


def _binop(op, ty):
    n = f"{op}_{_n(ty)}"
    return (n, f"define {ty} @{n}({ty} %a, {ty} %b) {{\n"
               f"  %r = {op} {ty} %a, %b\n  ret {ty} %r\n}}\n")


def _fbinop(op, ty):
    n = f"{op}_{_n(ty)}"
    return (n, f"define {ty} @{n}({ty} %a, {ty} %b) {{\n"
               f"  %r = {op} {ty} %a, %b\n  ret {ty} %r\n}}\n")


# Intrinsics spell float types f32/f64, not the IR type keywords.
_FLOAT_SUFFIX = {"float": "f32", "double": "f64", "x86_fp80": "f80"}


def _unary_call(intrinsic, ty):
    callee = intrinsic.lstrip("@")
    stem = callee.replace(".", "_")
    if stem.startswith("llvm_"):
        stem = stem[len("llvm_"):]
    n = stem
    return (n, f"define {ty} @{n}({ty} %a) {{\n"
               f"  %r = call {ty} @{callee}({ty} %a)\n  ret {ty} %r\n}}\n")


def _builtin2(intrinsic, ty):
    """A two-argument intrinsic call, e.g. llvm.smax.i32(i32, i32)."""
    callee = intrinsic.lstrip("@")
    stem = callee.replace(".", "_")
    if stem.startswith("llvm_"):
        stem = stem[len("llvm_"):]
    n = stem
    return (n, f"define {ty} @{n}({ty} %a, {ty} %b) {{\n"
               f"  %r = call {ty} @{callee}({ty} %a, {ty} %b)\n"
               f"  ret {ty} %r\n}}\n")


def _conv(op, src, dst):
    n = f"{op}_{_n(src)}_to_{_n(dst)}"
    return (n, f"define {dst} @{n}({src} %a) {{\n"
               f"  %r = {op} {src} %a to {dst}\n  ret {dst} %r\n}}\n")


def _icmp(pred, ty):
    n = f"icmp_{pred}_{_n(ty)}"
    return (n, f"define i1 @{n}({ty} %a, {ty} %b) {{\n"
               f"  %r = icmp {pred} {ty} %a, %b\n  ret i1 %r\n}}\n")


def _load(ty, align):
    n = f"load_{_n(ty)}_a{align}"
    return (n, f"define {ty} @{n}(ptr %p) {{\n"
               f"  %v = load {ty}, ptr %p, align {align}\n  ret {ty} %v\n}}\n")


def _store(ty, align):
    n = f"store_{_n(ty)}_a{align}"
    return (n, f"define void @{n}(ptr %p, {ty} %v) {{\n"
               f"  store {ty} %v, ptr %p, align {align}\n  ret void\n}}\n")


def _volatile_load(ty, align):
    n = f"load_volatile_{_n(ty)}"
    return (n, f"define {ty} @{n}(ptr %p) {{\n"
               f"  %v = load volatile {ty}, ptr %p, align {align}\n  ret {ty} %v\n}}\n")


def _volatile_store(ty, align):
    n = f"store_volatile_{_n(ty)}"
    return (n, f"define void @{n}(ptr %p, {ty} %v) {{\n"
               f"  store volatile {ty} %v, ptr %p, align {align}\n  ret void\n}}\n")


TEMPLATES = []

# --- scalar integer arithmetic, every width --------------------------------
for ty in ("i8", "i16", "i32", "i64", "i128"):
    for op in ("add", "sub", "mul", "and", "or", "xor", "shl", "lshr", "ashr"):
        TEMPLATES.append(_binop(op, ty))
# Widths that are not a power of two exercise the widen/narrow chains.
for ty in ("i1", "i7", "i24", "i65", "i256"):
    TEMPLATES.append(_binop("add", ty))
    TEMPLATES.append(_binop("and", ty))

# --- div/rem ---------------------------------------------------------------
for ty in ("i8", "i16", "i32", "i64"):
    for op in ("udiv", "sdiv", "urem", "srem"):
        TEMPLATES.append(_binop(op, ty))

# --- comparisons -----------------------------------------------------------
for ty in ("i8", "i16", "i32", "i64", "i128", "i65"):
    for pred in ("eq", "slt", "ult"):
        TEMPLATES.append(_icmp(pred, ty))

# --- bit manipulation ------------------------------------------------------
for ty in ("i16", "i32", "i64"):
    TEMPLATES.append(_unary_call("llvm.bswap." + ty, ty))
for ty in ("i32", "i64"):
    for fn in ("smax", "smin", "umax", "umin"):
        TEMPLATES.append(_builtin2(f"llvm.{fn}.{ty}", ty))
TEMPLATES.append((
    "uadd_overflow_i64",
    "define { i64, i1 } @uadd_overflow_i64(i64 %a, i64 %b) {\n"
    "  %r = call { i64, i1 } @llvm.uadd.with.overflow.i64(i64 %a, i64 %b)\n"
    "  ret { i64, i1 } %r\n}\n"))


# --- integer conversions ---------------------------------------------------
for src, dst in (("i64", "i8"), ("i64", "i16"), ("i64", "i32"),
                 ("i32", "i8"), ("i32", "i16"), ("i128", "i32"),
                 ("i128", "i64"), ("i65", "i32"), ("i256", "i64"),
                 ("i24", "i8")):
    TEMPLATES.append(_conv("trunc", src, dst))
for op in ("sext", "zext"):
    for src, dst in (("i8", "i16"), ("i8", "i32"), ("i8", "i64"),
                     ("i16", "i32"), ("i16", "i64"), ("i32", "i64"),
                     ("i7", "i32"), ("i24", "i64"), ("i1", "i32")):
        TEMPLATES.append(_conv(op, src, dst))

# --- floating point --------------------------------------------------------
for ty in ("float", "double"):
    for op in ("fadd", "fsub", "fmul", "fdiv"):
        TEMPLATES.append(_fbinop(op, ty))
    TEMPLATES.append((
        f"fneg_{_n(ty)}",
        f"define {ty} @fneg_{_n(ty)}({ty} %a) {{\n"
        f"  %r = fneg {ty} %a\n  ret {ty} %r\n}}\n"))
    TEMPLATES.append(_unary_call(f"llvm.sqrt.{_FLOAT_SUFFIX[ty]}", ty))
    TEMPLATES.append(_unary_call(f"llvm.fabs.{_FLOAT_SUFFIX[ty]}", ty))
    TEMPLATES.append((
        f"fcmp_olt_{_n(ty)}",
        f"define i1 @fcmp_olt_{_n(ty)}({ty} %a, {ty} %b) {{\n"
        f"  %r = fcmp olt {ty} %a, %b\n  ret i1 %r\n}}\n"))
for op in ("fadd", "fmul"):
    TEMPLATES.append(_fbinop(op, "x86_fp80"))

# --- int <-> float ---------------------------------------------------------
for ity in ("i8", "i16", "i32", "i64"):
    for fty in ("float", "double"):
        TEMPLATES.append(_conv("sitofp", ity, fty))
        TEMPLATES.append(_conv("uitofp", ity, fty))
        TEMPLATES.append(_conv("fptosi", fty, ity))
        TEMPLATES.append(_conv("fptoui", fty, ity))
TEMPLATES.append(_conv("fpext", "float", "double"))
TEMPLATES.append(_conv("fptrunc", "double", "float"))

# --- vectors ---------------------------------------------------------------
# G_MUL on a vector needs a BUILD_VECTOR that x86 ISel cannot select for
# every lane count, so only the shape the pipeline finishes is kept.
for ty in ("<2 x i32>", "<4 x i32>", "<8 x i16>", "<16 x i8>", "<32 x i8>",
           "<3 x i32>"):
    for op in ("add", "sub", "and", "or", "xor"):
        TEMPLATES.append(_binop(op, ty))
TEMPLATES.append(_binop("mul", "<8 x i16>"))
for ty in ("<4 x float>", "<2 x double>"):
    for op in ("fadd", "fsub", "fmul"):
        TEMPLATES.append(_fbinop(op, ty))

# --- memory ----------------------------------------------------------------
for ty, align in (("i8", 1), ("i16", 2), ("i32", 4), ("i64", 8), ("i128", 8),
                  ("float", 4), ("double", 8), ("x86_fp80", 16),
                  ("<4 x i32>", 4), ("<8 x i16>", 2), ("<4 x float>", 4),
                  ("<2 x double>", 8)):
    TEMPLATES.append(_load(ty, align))
    TEMPLATES.append(_store(ty, align))
TEMPLATES.append(_volatile_load("i32", 4))
TEMPLATES.append(_volatile_store("i64", 8))
TEMPLATES.append(_load("i64", 1))   # unaligned: the store/load alignment sets
TEMPLATES.append(_store("i32", 1))
TEMPLATES.append(_load("i32", 2))
TEMPLATES.append(_store("i64", 2))

# --- pointers and control flow --------------------------------------------
TEMPLATES.append((
    "ptrtoint_i64",
    "define i64 @ptrtoint_i64(ptr %p) {\n"
    "  %r = ptrtoint ptr %p to i64\n  ret i64 %r\n}\n"))
TEMPLATES.append((
    "inttoptr",
    "define ptr @inttoptr(i64 %v) {\n"
    "  %r = inttoptr i64 %v to ptr\n  ret ptr %r\n}\n"))
TEMPLATES.append((
    "gep_i8",
    "define ptr @gep_i8(ptr %p, i64 %i) {\n"
    "  %r = getelementptr i8, ptr %p, i64 %i\n  ret ptr %r\n}\n"))
TEMPLATES.append((
    "alloca_store_load",
    "define i64 @alloca_store_load(i64 %v) {\n"
    "  %p = alloca i64\n  store i64 %v, ptr %p, align 8\n"
    "  %r = load i64, ptr %p, align 8\n  ret i64 %r\n}\n"))
TEMPLATES.append((
    "select_i32",
    "define i32 @select_i32(i1 %c, i32 %a, i32 %b) {\n"
    "  %r = select i1 %c, i32 %a, i32 %b\n  ret i32 %r\n}\n"))
TEMPLATES.append((
    "select_i64",
    "define i64 @select_i64(i1 %c, i64 %a, i64 %b) {\n"
    "  %r = select i1 %c, i64 %a, i64 %b\n  ret i64 %r\n}\n"))
TEMPLATES.append((
    "phi_i64",
    "define i64 @phi_i64(i1 %c, i64 %a, i64 %b) {\n"
    "entry:\n  br i1 %c, label %t, label %e\n"
    "t:\n  br label %m\n"
    "e:\n  br label %m\n"
    "m:\n  %r = phi i64 [ %a, %t ], [ %b, %e ]\n  ret i64 %r\n}\n"))
TEMPLATES.append((
    "brcond",
    "define void @brcond(i1 %c) {\n  br i1 %c, label %t, label %e\n"
    "t:\n  ret void\ne:\n  ret void\n}\n"))
TEMPLATES.append((
    "switch_i32",
    "define void @switch_i32(i32 %x) {\n"
    "  switch i32 %x, label %d [ i32 0, label %a\n i32 1, label %b ]\n"
    "a:\n  ret void\nb:\n  ret void\nd:\n  ret void\n}\n"))
TEMPLATES.append((
    "freeze_i32",
    "define i32 @freeze_i32(i32 %a) {\n  %r = freeze i32 %a\n  ret i32 %r\n}\n"))


HEADER = """\
; Legalizer-table benchmark corpus for x86_64 GlobalISel.
;
; Generated by llvm/utils/gisel-table-bench/gen_ir.py -- do not edit by hand.
; Every function here is one the X86 GlobalISel legalizer fully legalizes, so
; the measured legalizer work is the whole corpus and not a prefix of it.
"""


def emit(copies, out, templates):
    out.write(HEADER)
    out.write("\n")
    out.write(DECLS)
    out.write("\n")
    for c in range(copies):
        for name, body in templates:
            fn = f"{name}_{c}"
            out.write(body.replace("@" + name + "(", "@" + fn + "(", 1))
            out.write("\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--copies", type=int, default=1,
                    help="how many copies of each template to emit (default 1)")
    ap.add_argument("-o", "--output", default="-",
                    help="output file, or - for stdout (default -)")
    ap.add_argument("--templates", action="store_true",
                    help="list the template names and exit")
    ap.add_argument("--split-dir", metavar="DIR",
                    help="write one file per template into DIR and exit; used "
                         "to check small templates before the whole corpus")
    ap.add_argument("--filter", metavar="REGEX",
                    help="emit only templates whose name matches REGEX; used to "
                         "build a query mix that is shallow or deep in the rule "
                         "chains")
    ap.add_argument("--exclude", metavar="REGEX",
                    help="drop templates whose name matches REGEX")
    args = ap.parse_args()

    templates = TEMPLATES
    if args.filter:
        import re
        want = re.compile(args.filter)
        templates = [(n, b) for n, b in templates if want.search(n)]
    if args.exclude:
        import re
        drop = re.compile(args.exclude)
        templates = [(n, b) for n, b in templates if not drop.search(n)]
    if not templates:
        sys.stderr.write("gen_ir.py: the filter matched no template\n")
        return 1

    if args.templates:
        for name, _ in templates:
            print(name)
        return 0

    if args.split_dir:
        os.makedirs(args.split_dir, exist_ok=True)
        for name, body in templates:
            with open(os.path.join(args.split_dir, name + ".ll"), "w") as f:
                f.write(DECLS + "\n" + body)
        return 0

    if args.output == "-":
        emit(args.copies, sys.stdout, templates)
    else:
        with open(args.output, "w") as f:
            emit(args.copies, f, templates)
    return 0


if __name__ == "__main__":
    sys.exit(main())
