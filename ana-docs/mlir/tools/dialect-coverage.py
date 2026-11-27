#!/usr/bin/env python3
"""Coverage audit: corpus op set vs the upstream TD mnemonics of one dialect.

The corpus is a *model* of a dialect, but it is only evidence for the grammar
if it is complete: an op that never appears is a construct the grammar was
never tested against.  This tool makes the gap explicit and countable.

Mapping is per dialect and explicit -- guessing a dialect's files from its
directory would silently include the transform op sets, whose ops belong to
the `transform` dialect, not to the dialect whose directory they live in.

Usage: tools/dialect-coverage.py [--verbose]
Exit status is 1 when any dialect file is missing ops.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CORPUS = ROOT / "corpus"
INC = ROOT.parent.parent / "mlir/include/mlir/Dialect"

# dialect keyword -> TD files (relative to mlir/include/mlir/Dialect)
TD = {
    "acc": ["OpenACC/OpenACCOps.td", "OpenACC/OpenACCCGOps.td"],
    "affine": ["Affine/IR/AffineOps.td"],
    "amdgpu": ["AMDGPU/IR/AMDGPUOps.td"],
    "arith": ["Arith/IR/ArithOps.td"],
    "arm_neon": ["ArmNeon/ArmNeon.td"],
    "arm_sme": ["ArmSME/IR/ArmSMEOps.td", "ArmSME/IR/ArmSMEIntrinsicOps.td"],
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
    "linalg": ["Linalg/IR/LinalgOps.td", "Linalg/IR/LinalgStructuredOps.td"],
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
    "smt": ["SMT/IR/SMTBitVectorOps.td", "SMT/IR/SMTIntOps.td",
            "SMT/IR/SMTArrayOps.td", "SMT/IR/SMTOps.td"],
    "sparse_tensor": ["SparseTensor/IR/SparseTensorOps.td"],
    "spirv": ["SPIRV/IR/SPIRVOps.td"],
    "tensor": ["Tensor/IR/TensorOps.td"],
    "tosa": ["Tosa/IR/TosaOps.td", "Tosa/IR/TosaShapeOps.td",
              "Tosa/IR/TosaUtilOps.td"],
    "transform": ["Transform/IR/TransformOps.td"],
    "ub": ["UB/IR/UBOps.td"],
    "vcix": ["LLVMIR/VCIXOps.td"],
    "vector": ["Vector/IR/VectorOps.td"],
    "wasmssa": ["WasmSSA/IR/WasmSSAOps.td"],
    "x86": ["X86/X86.td"],
    "xegpu": ["XeGPU/IR/XeGPUOps.td"],
    "xevm": ["LLVMIR/XeVMOps.td"],
}

# Mnemonics come from `def`/`defm` records whose base class is given a printed
# name: `def FooOp : Base<"foo", ...>`, `defm Foo : Base<"foo">`, possibly
# across lines.  Template parameters built from other defs are not mnemonics.
#   A record is an op when its name or its base class ends in `Op`; enum cases,
#   interfaces and typedefs share the `def X : Y<"...">` shape but are not ops.
#   Printed mnemonics are lowercase; this drops C++ strings and enum case names.
RECORD = re.compile(r'(?ms)\b(?:def|defm)\s+([A-Za-z_]\w*)\s*:\s*([A-Za-z_][\w:]*)'
                    r'\s*<\s*(?:[A-Za-z_][\w:]*\s*,\s*)?"([^"]+)"')
MNEMONIC_OK = re.compile(r"^[a-z][a-z0-9_.]*$")


def upstream(dialect):
    names = set()
    for rel in TD[dialect]:
        p = INC / rel
        if not p.exists():
            p = ROOT.parent.parent / "mlir/include/mlir/Dialect" / rel
        files = sorted(p.parent.glob(p.name))
        for f in files:
            for rec, base, mnem in RECORD.findall(f.read_text(errors="ignore")):
                if not (rec.endswith("Op") or base.endswith("Op")):
                    continue
                if MNEMONIC_OK.match(mnem):
                    names.add(mnem)
    return names


def main():
    verbose = "--verbose" in sys.argv
    bad = 0
    print("## coverage: upstream TD mnemonics vs corpus ops")
    print("| dialect | upstream | corpus | missing | extra |")
    print("| --- | --- | --- | --- | --- |")
    details = []
    for f in sorted(CORPUS.glob("*.dialect")):
        d = re.search(r"(?m)^\s*dialect\s+([a-z0-9_]+)", f.read_text()).group(1)
        if d not in TD:
            print("| %s | ? | ? | ? | ? |" % d)
            continue
        up = upstream(d)
        corpus = set(re.findall(r'(?m)^  op "?([a-z0-9_.]+)"?', f.read_text()))

        # 名字按"末段"对齐：intrinsic 族在语料里写作 `intr.ld1b.horiz`，
        # 上游助记符是 `ld1b.horiz`；同一条 op，不该算缺失。
        def covered(m):
            if m in corpus:
                return True
            return any(c.endswith("." + m) or m.endswith("." + c) for c in corpus)

        missing = sorted(m for m in up if not covered(m))
        extra = sorted(c for c in corpus
                       if not any(c == m or c.endswith("." + m) or m.endswith("." + c)
                                  for m in up))
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
    print("\n## dialects with a corpus but no TD mapping: see TD table")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
