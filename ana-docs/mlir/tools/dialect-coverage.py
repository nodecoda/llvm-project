#!/usr/bin/env python3
"""Coverage audit: corpus op set vs the dialect's upstream op set.

The corpus is a *model* of a dialect, but it is only evidence for the grammar
if it is complete: an op that never appears is a construct the grammar was
never tested against.  This tool makes the gap explicit and countable.

Ground truth is `llvm-tblgen --dump-json`: every `opName` a TableGen include
closure actually produces.  A text scan of the `.td` files cannot do this,
because mnemonics are routinely built by concatenation
(`NVVM_Op<"convert." # !tolower(src) # "x2.to.f4x2">`), by `multiclass`
expansion, or by `foreach`; a regex sees the class parameter, not the op.

Mapping is per dialect and points at the *aggregate* TD file whose include
closure is exactly that dialect: `LinalgAllOps.td`, `SMT.td`,
`ArmSMEAllOps.td`, `SPIRVOps.td`.  Guessing a dialect's files from its
directory would pull in transform op sets, whose ops belong to `transform`.

Usage:
  LLVM_TBLGEN=/path/to/llvm-tblgen \\
  LLVM_BUILD_DIR=/path/to/build python3 tools/dialect-coverage.py [--verbose]

Without a tblgen binary the tool falls back to a text scan and says so; the
fallback under-reports concatenated and `foreach`-generated ops, so a green
fallback run is weaker evidence than a green tblgen run.
Exit status is 1 when any dialect file is missing ops.
"""

import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
REPO = ROOT.parent.parent
CORPUS = ROOT / "corpus"
INC = REPO / "mlir/include/mlir/Dialect"

# dialect keyword -> aggregate TD entry points (relative to mlir/include/mlir/Dialect)
TD = {
    "acc": ["OpenACC/OpenACCOps.td"],
    "affine": ["Affine/IR/AffineOps.td"],
    "amdgpu": ["AMDGPU/IR/AMDGPUOps.td"],
    "arith": ["Arith/IR/ArithOps.td"],
    "arm_neon": ["ArmNeon/ArmNeon.td"],
    "arm_sme": ["ArmSME/IR/ArmSMEAllOps.td"],
    "arm_sve": ["ArmSVE/IR/ArmSVE.td"],
    "async": ["Async/IR/AsyncOps.td"],
    "bufferization": ["Bufferization/IR/BufferizationOps.td"],
    "builtin": ["../IR/BuiltinOps.td"],
    "cf": ["ControlFlow/IR/ControlFlowOps.td"],
    "complex": ["Complex/IR/ComplexOps.td"],
    "dlti": ["DLTI/DLTIAttrs.td"],
    "emitc": ["EmitC/IR/EmitC.td"],
    "func": ["Func/IR/FuncOps.td"],
    "gpu": ["GPU/IR/GPUOps.td"],
    "index": ["Index/IR/IndexOps.td"],
    "irdl": ["IRDL/IR/IRDLOps.td"],
    "linalg": ["Linalg/IR/LinalgAllOps.td"],
    "llvm": ["LLVMIR/LLVMOps.td", "LLVMIR/LLVMIntrinsicOps.td"],
    "math": ["Math/IR/MathOps.td"],
    "memref": ["MemRef/IR/MemRefOps.td"],
    "ml_program": ["MLProgram/IR/MLProgramOps.td"],
    "mpi": ["MPI/IR/MPIOps.td"],
    "nvgpu": ["NVGPU/IR/NVGPUOps.td"],
    "nvvm": ["LLVMIR/NVVMOps.td"],
    "omp": ["OpenMP/OpenMPOps.td"],
    "pdl": ["PDL/IR/PDLOps.td"],
    "pdl_interp": ["PDLInterp/IR/PDLInterpOps.td"],
    "ptr": ["Ptr/IR/PtrOps.td"],
    "quant": ["Quant/IR/QuantOps.td"],
    "rocdl": ["LLVMIR/ROCDLOps.td"],
    "scf": ["SCF/IR/SCFOps.td"],
    "shape": ["Shape/IR/ShapeOps.td"],
    "shard": ["Shard/IR/ShardOps.td"],
    "smt": ["SMT/IR/SMT.td"],
    "sparse_tensor": ["SparseTensor/IR/SparseTensorOps.td"],
    "spirv": ["SPIRV/IR/SPIRVOps.td"],
    "standalone": ["mlir/examples/standalone/include/Standalone/StandaloneOps.td"],
    "tensor": ["Tensor/IR/TensorOps.td"],
    "toy": ["mlir/examples/toy/Ch7/include/toy/Ops.td"],
    "tosa": ["Tosa/IR/TosaOps.td"],
    "transform": ["Transform/IR/TransformOps.td"],
    "ub": ["UB/IR/UBOps.td"],
    "vcix": ["LLVMIR/VCIXOps.td"],
    "vector": ["Vector/IR/VectorOps.td"],
    "wasmssa": ["WasmSSA/IR/WasmSSAOps.td"],
    "x86": ["X86/X86.td"],
    "xegpu": ["XeGPU/IR/XeGPUOps.td"],
    "xevm": ["LLVMIR/XeVMOps.td"],
}

# Mnemonics print lowercase except SPIR-V's `spirv.FAdd`.
MNEMONIC_OK = re.compile(r"^[A-Za-z][A-Za-z0-9_.]*$")


def find_tblgen():
    for cand in (os.environ.get("LLVM_TBLGEN"),
                 shutil.which("llvm-tblgen"),
                 shutil.which("mlir-tblgen")):
        if cand and Path(cand).exists():
            return Path(cand)
    return None


def include_dirs():
    dirs = [REPO / "mlir/include",
            REPO / "mlir/include/mlir/Dialect/ArmSME/IR",
            REPO / "mlir/examples/standalone/include",
            REPO / "mlir/examples/toy/Ch7/include"]
    build = os.environ.get("LLVM_BUILD_DIR")
    if build:
        dirs += [Path(build) / "include", Path(build) / "tools/mlir/include"]
    return [d for d in dirs if d.is_dir()]


def resolve(rel):
    for base in (REPO, INC):
        p = base / rel
        if p.exists():
            return p
    return INC / rel


def tblgen_ops(tblgen, files):
    """Return ({opName: recordName}, failed).  The record name is what
    escape-audit needs to find the definition text of a generated op."""
    ops, failed = {}, []
    incs = include_dirs()
    for rel in files:
        for f in sorted(resolve(rel).parent.glob(resolve(rel).name)):
            cmd = [str(tblgen)]
            for d in incs:
                cmd += ["-I", str(d)]
            cmd += ["--dump-json", str(f)]
            r = subprocess.run(cmd, capture_output=True, text=True)
            if r.returncode != 0:
                failed.append((f.name, r.stderr.strip().splitlines()[:1]))
                continue
            for rec, v in json.loads(r.stdout).items():
                if isinstance(v, dict) and "opName" in v:
                    ops[v["opName"]] = (rec, v.get("assemblyFormat"),
                                        bool(v.get("hasCustomAssemblyFormat")))
    return ops, failed


def tblgen_names(tblgen, files):
    ops, failed = tblgen_ops(tblgen, files)
    return set(ops), failed


def op_names(dialect):
    """Authoritative opName set for a dialect, or None when tblgen is absent."""
    tblgen = find_tblgen()
    if not tblgen:
        return None
    names, _ = tblgen_names(tblgen, TD[dialect])
    return names


RECORD = re.compile(r'(?ms)\b(?:def|defm)\s+([A-Za-z_]\w*)\s*:\s*([A-Za-z_][\w:]*)'
                    r'\s*<\s*(?:[A-Za-z_][\w:]*\s*,\s*)?"([^"]+)"')


def fallback_names(files):
    """Text scan: sees literal `Base<"mnemonic">` only.  Under-reports."""
    names = set()
    for rel in files:
        for f in sorted(resolve(rel).parent.glob(resolve(rel).name)):
            for rec, base, mnem in RECORD.findall(f.read_text(errors="ignore")):
                if not (rec.endswith("Op") or base.endswith("Op")):
                    continue
                if MNEMONIC_OK.match(mnem):
                    names.add(mnem)
    return names


def main():
    verbose = "--verbose" in sys.argv
    tblgen = find_tblgen()
    engine = "tblgen opName" if tblgen else "text scan (fallback)"
    print("## coverage vs upstream (%s)" % engine)
    print("| dialect | upstream | corpus | missing | extra |")
    print("| --- | --- | --- | --- | --- |")
    bad, details = 0, []
    for f in sorted(CORPUS.glob("*.dialect")):
        text = f.read_text()
        d = re.search(r"(?m)^\s*dialect\s+([a-z0-9_]+)", text).group(1)
        if d not in TD:
            print("| %s | ? | ? | ? | ? |" % d)
            continue
        if tblgen:
            up, failed = tblgen_names(tblgen, TD[d])
            if failed:
                print("  ! %s: tblgen failed on %s" % (d, failed[0][0]))
                print("    " + (failed[0][1][0] if failed[0][1] else ""))
        else:
            up = fallback_names(TD[d])
        corpus = set(re.findall(r'(?m)^  op "?([A-Za-z0-9_.]+)"?', text))

        # 严格集合相等：tblgen 的 opName 就是打印助记符（含 `intr.` 前缀），
        # 语料声明名必须逐字相同。不做后缀对齐——那会让 `scalable.extract`
        # 被 `extract` 掩盖、`llvm.add` 被 `intr.vp.add` 掩盖。
        missing = sorted(up - corpus)
        extra = sorted(corpus - up)
        print("| %s | %d | %d | %d | %d |" % (d, len(up), len(corpus),
                                              len(missing), len(extra)))
        if missing:
            bad += 1
            details.append((d, missing))
        if extra and verbose:
            details.append((d + " (extra)", extra))
    if details:
        print("\n## missing ops")
        for d, names in details:
            print("- %s: %s" % (d, ", ".join(names)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
