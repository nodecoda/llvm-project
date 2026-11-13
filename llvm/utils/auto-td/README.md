# auto-td — TableGen records from an opcode database

`auto-td` turns *one structured YAML per instruction* plus *an authoritative
opcode database* into flat TableGen instruction records.  It exists so that
adding an instruction never means hand-writing nested TableGen classes and
hand-copying encoding bits.

This is an independent implementation.  It keeps the command line of the
reference tool (`--ysx-root/--riscv-opcodes/--ysx-opcodes/--out-dir/--coverage`
and the three `--clang-*`/`--llvm-intrinsics-td` outputs) so existing build
recipes port over, but shares no code with it.

## Design in one paragraph

Every bit of an instruction comes from the database; every operand and effect
comes from a shared taxonomy category; the YAML contributes only identity and
the few facts no database can know (which category, which predicate, which
intrinsic).  The generator therefore cannot invent an encoding, and a
contributor cannot smuggle one in — the YAML is scanned for TableGen tokens and
for encoding keys.

## Command line

```console
$ auto-td-gen \
    --target-root   llvm/lib/Target/<T> \
    --riscv-opcodes third_party/riscv-opcodes \
    --ysx-opcodes   third_party/<T>-opcodes \
    --out-dir       <build>/lib/Target/<T>/auto-td \
    --coverage      <build>/lib/Target/<T>/auto-td/AutoTdCoverage.md
```

| flag | meaning |
|---|---|
| `--target-root`, `--ysx-root` | directory containing `auto-td/` |
| `--riscv-opcodes` | upstream opcode database (encoding truth for standard instructions) |
| `--ysx-opcodes` | optional local opcode database (encoding truth for custom instructions) |
| `--out-dir` | where `AutoTdInstrInfo.inc` and `AutoTdManifests.inc` go |
| `--coverage` | coverage report path (defaults to `<out-dir>/AutoTdCoverage.md`) |
| `--clang-builtins-td`, `--clang-builtin-cg-inc`, `--llvm-intrinsics-td` | optional front-end artifacts |
| `--record-prefix` | override `record_prefix` from config |
| `--check-only` | validate and exit (no files written) |
| `--strict` | treat unused YAML keys as errors |

Exit code is non-zero when validation fails; all problems are reported at once.

## Data layout

```
<T>/auto-td/
  config.yaml                 # predicates, operand class mapping, formats
  instructions/<domain>/*.yaml
  taxonomy/*.yaml             # shared operand/effect categories
```

### Instruction YAML

```yaml
mnemonic: vadd.vv
opcode: {repo: riscv-opcodes, extension: rv_v, key: vadd_vv}   # or opcode_source:
spec: tinyv.vector-alu.int_add                                  # or spec_ref:
domain: tiny-v              # optional, defaults to the parent directory
predicate: HasStdExtXTinyV  # optional, defaults to config domains.<domain>
status: auto_full
aliases: [{mnemonic: vfredsum.vs}]
pseudos:  {matrix: {element_types: [i32], lmuls: standard, masked: true, policy: llvm_default}}
patterns: [{kind: intrinsic_to_pseudo, intrinsic: ysx.vadd, operation: add}]
builtin:  {header: ysx_vector.h, names: [ysx_vadd_vv_i32m1], codegen: true}
```

Note there is **no** encoding field.  `encoding`, `fixed_bits` and `raw_cpp` are
rejected outright.

Keys that carry instruction *semantics* (as opposed to shape) are:

| key | effect |
|---|---|
| `predicate` | `Predicates = [...]`; a string or a list (`[HasStdExtZmmul, IsRV64]`) |
| `sched` | `SchedRW = [...]`; `[]` is a *declared* empty schedule |
| `flags` | extra `let <flag> = 1;` lines (`isCommutable`, `IsSignExtendingOpW`, `hasNoSchedulingInfo`, …) |
| `fixed` | pin a database field to a constant (aq/rl variants, `fence` fields) |
| `format` | override the inferred instruction format |
| `nullary` | assert that a category with no operands is intentional (ecall/ebreak) |

`predicate: null` and a missing `predicate` are different: the first says
"this instruction genuinely has no predicate", the second is an error unless
`config.domains.<domain>` supplies one.  The same distinction applies to
`sched`: a missing key means "not declared" and (with
`emit_scheduling_stub: true`) produces `hasNoSchedulingInfo = 1`, while
`sched: []` produces an empty `SchedRW`.

### Taxonomy

```yaml
categories:
  int_add:
    operands:
      outs: [{role: dest, field: vd, reg_class: VR}]
      ins:
        - {role: lhs, field: vs2, reg_class: VR}
        - {role: rhs, field: vs1, reg_class: VR}
        - {role: mask_policy, field: vm, operand: VMaskOp}
    effects: {implicit_uses: [VL, VTYPE]}
```

`reg_class` names a TableGen register class; `operand` names an operand class.
Roles `base` + `offset` fold into `offset(base)` in the asm string.

### config.yaml

```yaml
record_prefix: YSX_AUTO      # "" emits canonical backend names
base_class: RVInst
domains:
  tiny-f: {predicate: HasStdExtXTinyF}
  tiny-v: {predicate: HasStdExtXTinyV}
operand_classes: {VMaskOp: YSXAutoVMaskOp, VMaskCarryInOp: YSXAutoVMaskCarryInOp}
formats: {vsetvli: InstFormatI}
emit_scheduling_stub: true
builtins: {types: {i32m1: "_ExtVector<4, int>"}, features: "xtinyv,zvl128b", class: YSXBuiltin}
```

## Encoding rules

Variable fields are emitted as themselves.  A handful of database names are
not "the operand of the same name" and have a rule, declared in
`autotd/encoding.py:FIELD_RULES` rather than hard-coded in an if-chain:

| field | rule |
|---|---|
| `nf` | forced to zero (unit-stride only) |
| `imm12hi` / `imm12lo` | slices `imm12{11-5}` / `imm12{4-0}` of one 12-bit operand |
| `bimm12hi` | `imm12{11}`, `imm12{9-4}` (bits 31, 30-25) |
| `bimm12lo` | `imm12{3-0}`, `imm12{10}` (bits 11-8, 7) |
| `jimm20` | `imm20{19}`, `imm20{9-0}`, `imm20{10}`, `imm20{18-11}` |
| `shamtd` / `shamtw` | the 6-bit / 5-bit `shamt` operand |
| `csr` | the 12-bit `imm12` operand (Zicsr reuses the I-format immediate slot) |

The B/J rules use the *instruction* variable (`imm12`, `imm20`), not the
13-/21-bit value with its implicit zero LSB: the backend's `RVInstB`/`RVInstJ`
classes declare the 12-/20-bit variable and scatter it themselves, so using the
larger width would produce different records.  When a rule covers a whole
variable the bare name is emitted (`imm12`, not `imm12{11-0}`).

Vector fields need no rule of their own: `arg_lut.csv` already carries `vd`,
`vs1`/`vs2`/`vs3`, `vm` (bit 25), `nf`, `simm5`, `zimm5`, `zimm10` and
`zimm11`, so the generator emits them as-is.  `nf` is the one exception and is
forced to zero, which is correct for the unit-stride forms a trimmed backend
needs; segment forms would have to replace it with a real operand.

Two more rules are structural, not name-based:

* **carry-input mask** — if the taxonomy declares an operand whose class is a
  carry-input mask, the constant `0` the database puts in that bit slot is
  replaced by the operand, and the asm string gets `, $vm` instead of `$vm`.
* **format inference** — `imm12hi`+`imm12lo` → `InstFormatS`,
  `bimm12hi`+`bimm12lo` → `InstFormatB`, `jimm20` → `InstFormatJ`,
  `imm12`/`shamtd`/`shamtw`/`csr` → `InstFormatI`, `imm20` → `InstFormatU`,
  otherwise `InstFormatR`, with per-mnemonic overrides from `config.formats`.

## Validation

Beyond "the opcode exists and the fields add up", the checker enforces:

* every database field of the row is either an operand, `fixed`, or covered by
  a field rule — nothing is silently dropped;
* no YAML carries TableGen (`bits<`, `let Inst{`, `multiclass`, `def : Pat`);
* `status` is one of the three known values, and `retained_schema_gap`
  names the files that still own the instruction;
* one database row may back several records only when they are the base
  mnemonic or `<base>.<suffix>` and each pins a different `fixed` set — this is
  what makes the four aq/rl variants of one atomic legal without making
  duplicates legal;
* record names are unique after the prefix is applied;
* every row of `coverage.extensions` is claimed by an instruction YAML, or is
  named in `coverage.retained_handwritten` (a typo there is an error);
* every standard record referenced from the target's C++ or by an
  `InstAlias` is still generated.

## Outputs

| file | content |
|---|---|
| `AutoTdInstrInfo.inc` | one flat `def` per instruction, plus `MnemonicAlias` lines |
| `AutoTdManifests.inc` | audit manifest (pseudo matrix / patterns / builtins), comments only |
| `AutoTdCoverage.md` | per-instruction status table |
| `--clang-builtins-td` | `def ... : <BuiltinClass><"prototype", "features">` |
| `--clang-builtin-cg-inc` | `case` dispatch into `CGM.getIntrinsic` |
| `--llvm-intrinsics-td` | intrinsic declarations |

## CMake wiring

```cmake
add_custom_command(
  OUTPUT ${AUTO_TD_DIR}/AutoTdInstrInfo.inc ${AUTO_TD_DIR}/AutoTdManifests.inc
  COMMAND ${Python3_EXECUTABLE} ${LLVM_MAIN_SRC_DIR}/../llvm/utils/auto-td/auto-td-gen
          --target-root ${CMAKE_CURRENT_SOURCE_DIR}
          --riscv-opcodes ${LLVM_MAIN_SRC_DIR}/../third_party/riscv-opcodes
          --out-dir ${AUTO_TD_DIR} --coverage ${AUTO_TD_DIR}/AutoTdCoverage.md
  DEPENDS ${AUTO_TD_DEPS}
  COMMENT "Generating auto-td instruction records")
```

The generated file must exist before TableGen parses the target, so the target
also has to `set(LLVM_TARGET_DEPENDS <generated file>)`.

## Verification

```console
$ python3 -m unittest discover -s llvm/utils/auto-td/tests -p 'test_*.py'
Ran 54 tests ... OK
```

The suite covers the opcode reader, every encoding rule (including the B/J/S
scatter rules), the vector shapes (`v_vv`/`v_vx`/`v_vi`, unit-stride
load/store, `vsetvli`/`vsetivli`/`vsetvl`, whole-register move, reduction and
compare), record naming, declared-empty `predicate`/`sched`, the emitter, the
guards, and the CLI.

Four external cross-checks back the claims:

* running the tool over the reference corpus (50 instructions) reproduces the
  reference tool's output byte for byte (`AutoTdInstrInfo.inc` and
  `AutoTdManifests.inc`);
* the RVTrim corpus (**535 instructions**: 160 scalar + 375 vector)
  reproduces the backend's own records:
  `535 instructions compared, 0 mismatching, 0 not in reference` with
  `tests/compare_riscv_records.py`, which compares every `Inst` bit after
  canonicalising record-local variable names;
* the 160 scalar instructions assemble to byte-identical objects (and
  disassemble identically) under `-triple=riscv64` and `-triple=rvtrim64`;
* the 375 vector records reproduce every generated property, not just the
  encoding bits: `tests/compare_record_props.py` reports
  `375 records compared, 0 mismatching, 0 missing` against the checked-in
  `auto-td/golden/v-all-props.txt` (predicate, `SchedRW`, `Uses`/`Defs`,
  `Constraints`, `DestEEW`, `VS1VS2Constraint`, `ElementsDependOn`,
  `ReadsPastVL`, `mayRaiseFPException`, `VMConstraint`, …).

The coverage gate in `--check-only` also enforces that every row of the
extensions listed in `config.yaml` has a YAML instruction (or is named in
`retained_handwritten`), and that every standard record referenced from the
target's C++ or by an `InstAlias` is still generated.  For RVTrim the retained
list is empty.

## Deliberate limitations

* Emitted records carry no `Pattern`; instruction selection stays in C++.
* `hasNoSchedulingInfo = 1` by default (`emit_scheduling_stub: false` to drop it).
* The pseudo matrix is a manifest, not generated TableGen.
* Builtin prototypes are derived from `config.builtins.types`, not guessed from
  the instruction name.
* Vector *pseudo* instructions (`RVTrimInstrInfoVPseudos.td`) and selection
  patterns remain hand-written; only the 375 real V records are generated.
* `InstAlias`/`MnemonicAlias` spellings remain hand-written and reference the
  generated records by name.
* Floating-point V records are generated but cannot be assembled under the
  current `RV64IMA_Zve64x` profile; they are verified at record level only.
