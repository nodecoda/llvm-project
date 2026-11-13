# RVTrim instruction corpus for auto-td

RVTrim is an RV64IMA core: `I`, `M`, `A`, `Zicsr` and `Zifencei`, with every
other extension rejected at feature-validation time.  The instruction records
are not written by hand — all 536 of them are generated from this directory by
`llvm/utils/auto-td/auto-td-gen` and included as `auto-td/AutoTdInstrInfo.inc`
from `RVTrimInstrInfo.td`.  That corpus is 160 RV64IMA records, 375 vector
records (generated for coverage, but their features are rejected by
`RVTrimSubtarget`), and one vendor record.

Because `config.yaml` sets `record_prefix: ""`, the generated records carry the
canonical backend names (`ADDI`, `LR_W_AQ`, `MULW`, …): they *are* the backend's
definitions, not a parallel copy.  Adding `--record-prefix RVTRIM_AUTO` on the
command line emits a namespaced copy instead, which is how the corpus is
cross-checked against the hand-written RISCV records.

## Layout

```
auto-td/
  config.yaml                   # domains -> predicates, formats, base class
  taxonomy/core.yaml            # 17 operand/effect categories
  instructions/rv-i/        (37) lui auipc jal jalr branches loads stores ALU
  instructions/rv64-i/      (15) lwu ld sd slli/srli/srai addiw..sraw
  instructions/rv-m/         (8) mul mulh mulhsu mulhu div divu rem remu
  instructions/rv64-m/       (5) mulw divw divuw remw remuw
  instructions/rv-a/        (44) lr.w/sc.w/amo*.w x {-, .aq, .rl, .aqrl}
  instructions/rv64-a/      (44) lr.d/sc.d/amo*.d x {-, .aq, .rl, .aqrl}
  instructions/rv-zicsr/     (6) csrrw..csrrci
  instructions/rv-zifencei/  (1) fence.i
  instructions/rv-v/       (375) the Zve* vector corpus (feature-gated)
  instructions/rv-xtinyv/    (1) yushuxin.vfexp (vendor extension)
  opcodes/                      vendored riscv-opcodes subset (BSD-2-Clause)
  opcodes-ysx/extensions/       vendor opcode overlay (one row)
```

`opcodes/` is a verbatim subset of upstream `riscv-opcodes` (`arg_lut.csv`
plus the `extensions/` files the corpus uses) and keeps its `LICENSE`.  It is
vendored rather than fetched so a build has no network dependency and the
encoding truth for a release is pinned by the checkout.

`opcodes-ysx/` is a second, vendor-owned database.  It is passed with
`--ysx-opcodes` and lets a YAML name `repo: ysx-opcodes` for an instruction
that has no upstream row.  Field bit ranges come from the standard `arg_lut`
(fallback LUT), so a vendor file only needs the encoding line itself.

## Adding an instruction

1. Add `instructions/<domain>/<mnemonic>.yaml`:

   ```yaml
   mnemonic: andn
   opcode: {repo: riscv-opcodes, extension: rv_zbb, key: andn}
   spec: rvtrim.core.r_type
   status: auto_full
   predicate: HasStdExtZbb
   sched: [WriteIALU, ReadIALU, ReadIALU]
   ```

   `spec` names a category in `taxonomy/core.yaml`; the category decides which
   operand class fills which database field.  There is no encoding field — the
   bit pattern comes from the opcode row, and the checker rejects any YAML that
   tries to carry TableGen or fixed bits.

2. If the mnemonic needs an opcode file that is not vendored yet, copy it from
   upstream `riscv-opcodes/extensions/` into `opcodes/extensions/`.

3. Rebuild.  `CMakeLists.txt` globs `auto-td/*.yaml` with
   `CONFIGURE_DEPENDS`, so the custom command reruns automatically:

   ```console
   $ ninja lib/Target/RVTrim/RVTrimCommonTableGen
   ```

To add a *variant* of an existing row (the aq/rl pattern), add a second YAML
with the suffixed mnemonic and a `fixed` map that pins the differing bits:

```yaml
mnemonic: lr.w.aq
opcode: {repo: riscv-opcodes, extension: rv_a, key: lr_w}
spec: rvtrim.core.lr_type
status: auto_full
fixed: {aq: 1, rl: 0}
predicate: HasStdExtZalrsc
sched: [WriteAtomicLDW, ReadAtomicLDW]
flags: [IsSignExtendingOpW, mayLoad]
```

The checker accepts several records on one opcode row only when the mnemonics
are `<base>` or `<base>.<suffix>` and each record pins a different `fixed`
set.

## Vendor extensions

A vendor instruction is the same two-file addition as a standard one, plus one
opcode row in a separate database:

```yaml
# instructions/rv-xtinyv/yushuxin_vfexp.yaml
mnemonic: yushuxin.vfexp
opcode: {repo: ysx-opcodes, extension: rv_xtinyv, key: yushuxin_vfexp}
spec: rvtrim.vector.v_unary_nomask
domain: rv-xtinyv
status: auto_full
sched: [WriteVIALUV_WorstCase, ReadVIALUV_WorstCase]
```

```
# opcodes-ysx/extensions/rv_xtinyv
yushuxin.vfexp 31..26=0x2a vm vs2 19..15=0 14..12=0x1 vd 6..0=0x0b
```

`config.yaml` maps the domain to `HasVendorXTinyV` and lists
`ysx-opcodes/rv_xtinyv` under `coverage.extensions`, so the row cannot silently
lose its YAML.  The feature itself is declared in `RVTrimFeatures.td` as a
`FeatureVendor*` extension; without `+xtinyv` the assembler rejects the
mnemonic.

This instruction has no masked form.  It uses the `v_unary_nomask` category,
which declares no mask operand; the generator then pins the row's `vm` bit to
1 (`LITERAL_ONE_FIELDS`) instead of turning it into an operand.  Modelling it
as a masked instruction would produce a `v0.t` form the hardware does not
have.  `llvm/test/MC/RVTrim/xtinyv-vfexp.s` pins the encoding, the absence of
the masked form, and the predicate diagnostic.

The IR path is the hand-written semantic layer that sits on top of the
generated record:

* `llvm/include/llvm/IR/IntrinsicsRVTrim.td` declares
  `llvm.rvtrim.vfexp` under `TargetPrefix = "rvtrim"`.  A target-specific
  prefix needs its own `IntrinsicsRVTrim.h`, generated by the
  `-intrinsic-prefix=rvtrim` rule in `llvm/include/llvm/IR/CMakeLists.txt`;
  the shared `IntrinsicEnums.inc` does not contain prefixed intrinsics.
* `RVTrimISelDAGToDAG.cpp` selects the intrinsic to `YUSHUXIN_VFEXP` and passes
  only the source value, because the record has no mask operand.
* `llvm/test/CodeGen/RVTrim/xtinyv-vfexp.ll` checks IR -> `yushuxin.vfexp`.

The intrinsic is currently typed at LMUL=1 (`nxv2i32`, ABI class `v8`).  The
record uses `VR` (LMUL=1); a value whose ABI class is wider (for example
`nxv4i32`, passed in `v8m2`) would need an LMUL-converting copy that RVTrim
does not lower yet, so selection stays at LMUL=1 until that exists.

## What is generated and what is not

Generated: the flat `def` (operands, asm string, `Predicates`, `mayLoad`/
`mayStore`/`hasSideEffects`, `SchedRW`, flags, and every `Inst` bit).

Not generated, still hand-written in `RVTrimInstrInfo*.td`:

* `Pattern`s and instruction selection (`RVTrimInstrInfoM.td` etc.);
* pseudo instructions and their expansion;
* the extension definitions themselves (`RVTrimFeatures.td`,
  `RVTrimInstrInfoF/D/V/...`).  Those records exist but their features are
  rejected by `RVTrimSubtarget`, which is what keeps the profile at RV64IMA.

## Verifying the corpus

```console
# validation only: field coverage, predicates, duplicate rows
$ python3 llvm/utils/auto-td/auto-td-gen \
    --target-root llvm/lib/Target/RVTrim \
    --riscv-opcodes llvm/lib/Target/RVTrim/auto-td/opcodes \
    --ysx-opcodes llvm/lib/Target/RVTrim/auto-td/opcodes-ysx \
    --out-dir /tmp/rvtrim --check-only
auto-td: 536 instructions OK

# every Inst bit against the RISCV backend's own records
$ llvm-tblgen -print-records -I llvm/include -I llvm/lib/Target/RISCV \
    llvm/lib/Target/RISCV/RISCV.td > /tmp/riscv.records
$ python3 llvm/utils/auto-td/auto-td-gen \
    --target-root llvm/lib/Target/RVTrim \
    --riscv-opcodes llvm/lib/Target/RVTrim/auto-td/opcodes \
    --ysx-opcodes llvm/lib/Target/RVTrim/auto-td/opcodes-ysx \
    --out-dir /tmp/rvtrim --record-prefix RVTRIM_AUTO
$ python3 llvm/utils/auto-td/tests/compare_riscv_records.py \
    --inc /tmp/rvtrim/AutoTdInstrInfo.inc --records /tmp/riscv.records \
    --strip-prefix RVTRIM_AUTO
535 instructions compared, 0 mismatching, 1 not in reference
```

The one record "not in reference" is the vendor instruction, which has no
upstream counterpart.  The floating-point part of the vector corpus is record-level only: those features
are rejected by the trimmed profile, so they cannot be assembled.

Lit coverage lives in `llvm/test/MC/RVTrim/` (encoding shapes, trimmed-profile
rejection, predicate requirements) and `llvm/test/CodeGen/RVTrim/` (ALU, memory,
atomics, calls, CFI and the vendor intrinsic through instruction selection).

The end-to-end check is behavioural: assemble a file covering the profile with
both targets and compare the objects.

```console
$ llvm-mc -triple=riscv64  -mattr=+m,+a,+zicsr,+zifencei -filetype=obj all.s -o a.o
$ llvm-mc -triple=rvtrim64 -mattr=+m,+a,+zicsr,+zifencei -filetype=obj all.s -o b.o
$ cmp a.o b.o && echo identical
```

## Deliberate gaps

* `fence`/`fence.i`/`ecall`/`ebreak` carry no `bits<>` field declarations for
  slots that are constant zero; the `Inst` bits are identical, but a textual
  diff against RISCV shows the missing declarations.
* `lr.w`/`lr.d` do not declare the always-zero `rs2` field for the same reason.
