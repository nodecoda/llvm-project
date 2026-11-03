# x86-legalizer-corpus

Build legalizer query corpora from the **hand-written** X86 legalizer C++
(`llvm/lib/Target/X86/GISel/X86LegalizerInfo.cpp`), at any git revision
(default `upstream/main`), then let an oracle fill in the expected answers.

This tool is deliberately not a TableGen reader.  It parses the source file
that a human wrote, so the corpus describes the chain semantics that the
generated table is supposed to replace.  Generated `.inc` files are read by
`verify` only, as a cross-check.

## Oracle boundary

The Python side never writes `expected`.  It emits query shapes only:

```
C++ source --parse--> rule ledger --points--> query shapes
                                                  |
                              X86Tests (reference) fills the answers
                                                  |
                                            corpus JSON
```

The only producer of an expected action is
`X86LegalizerInfoReference::getAction()` in the C++ unit test driver.
`points` output is therefore safe to diff and review: it contains no answers
that the table could have influenced.

## Source and oracle

The tool separates *where the shapes come from* from *where the answers come
from*:

| Piece | Default | Meaning |
|---|---|---|
| source | `--source-rev upstream/main` | the C++ file the query shapes come from |
| oracle | `none` by default; `reference` when `--build` is given | the only producer of `expected` |
| coverage exceptions | `coverage_exceptions.json` | reviewed reasons for rules that cannot be probed |

`--oracle=none` writes a shapes-only corpus: every atom is present, none has
an `expected` field.  `--oracle=reference` runs the frozen C++
`X86LegalizerInfoReference::getAction()` through `X86Tests` and is the only
mode that fills `expected`.  A revision-specific `upstream-build` oracle is
not implemented; that is why the default for `upstream/main` is
`--oracle=none`.

`source.rev` records the requested spelling (`upstream/main`), while
`source.revision` records the resolved 40-character commit.  The oracle
records the resolved reference commit and, when it differs from the requested
spelling, its `spec`.

If `source.revision != oracle.revision` the corpus is cross-revision: the
envelope carries `"oracle": {"cross_revision": true, ...}` and
`--oracle=reference` refuses to run unless `--allow-cross-revision` is given.
With the flag, `<out>.report.json` records the rule delta by
`(rule_set, method, argument text)`, not by rule index.

## Quick start

```bash
# 1. Shapes from upstream/main, with no expected answers.  This is the
#    default workflow for a revision that has no matching oracle build.
python3 llvm/utils/x86-legalizer-corpus corpus \
    --source-rev upstream/main --oracle=none --tier witness \
    --out /tmp/up-shapes.json

# 2. Rule ledger for the same revision.
python3 llvm/utils/x86-legalizer-corpus parse \
    --source-rev upstream/main --config C2 --out /tmp/up-ast.json

# 3. Cross-check the hand-written chain against a TableGen emission.
python3 llvm/utils/x86-legalizer-corpus verify \
    --ast /tmp/up-ast.json \
    --td-inc build/unittests/Target/X86/X86LegalizerRuleIndex.inc \
    --out /tmp/up-report.json

# 4. Corpus with expected answers from the frozen C++ reference.
#    The source revision must match the reference unless you opt in.
python3 llvm/utils/x86-legalizer-corpus corpus \
    --source-rev ebac17721d12 --tier witness \
    --build /path/to/build --out /tmp/x86-corpus.json
```

`corpus` without `--build` writes only the points file (`<out>.points.json`)
and tells you what to do next.  Environment overrides for step 4 are
`X86_LEGALIZER_CPP_POINTS` and `X86_LEGALIZER_CPP_CORPUS_OUT`; they select the
input points and the output corpus for the gtest driver.

Step 3 exits 3 on `upstream/main`: TableGen has two `G_CTPOP` rules that the
main-branch C++ chain does not have (`HasVPOPCNTDQ`, `HasVPOPCNTDQVL`).  That
is the tool reporting a real source delta, not a corpus bug.

## Key -> result traces

`corpus` records one legalization decision per query: key -> action.  A
legalizer also cares about what happens next: apply the mutation, ask again,
repeat.  Two disabled gtest drivers replay a corpus that way, with the same
loop and a different oracle:

| Driver | Oracle |
|---|---|
| `*TraceCorpusWithReference*` | frozen C++ `X86LegalizerInfoReference` |
| `*TraceCorpusWithTable*` | `X86LegalizerTable` |

Both read `X86_LEGALIZER_TRACE_CORPUS_IN` and write
`X86_LEGALIZER_TRACE_CORPUS_OUT`; `X86_LEGALIZER_TRACE_MAX_ROUNDS` (default
64) is the cap after which a still-unresolved atom is reported as `cap`.

Each trace document records, per atom, the start state, the terminal `status`
(`legal`, `unsupported`, `custom`, `lower`, `libcall`, `cycle`, `cap`,
`invalid`), the final types and memory descriptor, and every step
(`action`, `idx`, `to`).  Table traces also record each step's `answer_kind`
(`hit`, `residual`, `escape`, `outside`, `untabulated`).

The simulation applies only the type mutation named by the step.  Actions that
carry no type (Legal, Unsupported, Custom, Lower, Libcall) end the trace,
memory descriptors stay fixed, and a repeated `(types, memdesc)` state is
reported as `cycle`.  The simulator never invents an answer: every `status`
comes from the oracle under test.

One corpus, both oracles, one comparison:

```bash
llvm/utils/x86-legalizer-corpus/run-traces.sh \
    /tmp/x86-corpus.json /tmp/traces /path/to/build
```

It runs both drivers and then `trace-diff --strict`, which exits 1 on any
difference.  The same steps by hand:

```bash
# reference trace
X86_LEGALIZER_TRACE_CORPUS_IN=/tmp/x86-corpus.json \
X86_LEGALIZER_TRACE_CORPUS_OUT=/tmp/trace-reference.json \
  build/unittests/Target/X86/X86Tests --gtest_also_run_disabled_tests \
    --gtest_filter='*TraceCorpusWithReference*'

# table trace over the same corpus
X86_LEGALIZER_TRACE_CORPUS_IN=/tmp/x86-corpus.json \
X86_LEGALIZER_TRACE_CORPUS_OUT=/tmp/trace-table.json \
  build/unittests/Target/X86/X86Tests --gtest_also_run_disabled_tests \
    --gtest_filter='*TraceCorpusWithTable*'

# compare key -> result
python3 llvm/utils/x86-legalizer-corpus trace-diff \
    --left /tmp/trace-reference.json --right /tmp/trace-table.json \
    --out /tmp/trace-diff.json --strict
```

`trace-diff` joins by atom id, compares `rounds`, `status`, `final_types`,
`final_mem`, and the step list, and reports status/rounds histograms plus the
first divergent step for each difference.  `answer_kind` is provenance: it is
summarised, never counted as a difference.

## Tiers

| Tier | What it adds | Points on the frozen revision (C2) |
|---|---|---|
| `witness` | reviewed 12-point boundary panel, address-space probes, per-rule witnesses | 2219 |
| `full` | every class pair over the C++ class axis, plus the witness panel | 208853 |
| `deep` | `full` plus the reviewed 60-point deep panel | 319541 |

Tiers only change the query shapes.  They never change the expected answers,
which always come from the oracle.

`full` and `deep` prune class-pair and boundary shapes that put a pointer (or
pointer vector) in a slot targeted by an element-size mutation that can match
vectors of pointers (`clampScalarOrElt`, `scalarSameSizeAs`,
`min/maxScalarOrElt`).  The reference applies the first match's mutation, and
`LLT::changeElementSize` rejects pointers with an assertion, so those are not
well-formed legalizer queries.  The points document records the count under
`"pruned": {"pointer_element_size": N}` (858 for `full`, 2010 for `deep` on
the frozen revision).  This is a query-shape safety filter, not an oracle: it
never writes an expected action.

## Configs

`--config` selects a row of the `CONFIGS` table in `target.py`; the rows mirror
the four `FullConfigs` in `X86LegalizerInfoDifferentialTest.cpp`.

| Config | Shape |
|---|---|
| `C0` | 32-bit, no SSE/AVX, no x87 |
| `C1` | 64-bit, SSE2 only |
| `C2` | 64-bit, AVX-512 + BWI/DQI/VLX (witness config; every guard is taken) |
| `C3` | 32-bit with POPCNT/LZCNT/BMI, no SSE |

`verify` currently accepts only a `C2` AST.  The emitted rule index is the
unconditional TableGen translation, while `C0`/`C1`/`C3` prune guarded rules;
comparing another config would report every pruned rule as a delta.

## What the reader understands

The parser is a strict subset of C++, not a general interpreter:

- declarations of `LLT` aliases and integer constants;
- nested ternaries, arithmetic, comparisons and `Subtarget.<predicate>()`;
- `if`/`else` with literal pruning, including `auto &Action = ...` chains;
- `for (unsigned Op : {G_...})` expansion;
- the builder chain methods listed in `KNOWN_METHODS`;
- lambdas are recorded, not executed; simple literal predicates inside a
  lambda (`typeIs`, `typeInSet`, `typePairInSet`, `scalar*Than`, ...) are
  modelled to find a witness, and anything else stays unverified until it has
  a reviewed coverage exception.

Anything else is a hard error with a line number.  `--allow-opaque` downgrades
an unknown chain method to a recorded warning; use it only when you are
inspecting a new source revision, never for a committed corpus.

## Extending the tool

- **New chain method**: add it to `KNOWN_METHODS`, teach `record_shape()` which
  slots it reads, and add a case to `domain.probe_candidates()` and
  `domain.rule_matches()`.  The point of the ledger is that a silent no-op is
  impossible.
- **New lambda predicate**: extend `domain.pred_witness()` /
  `probe_from_lambda()` for a literal predicate, or record a reviewed
  exception.  A rule whose predicate cannot be modelled is never silently
  dropped.
- **New `Subtarget` predicate**: add it to `target.FEATURE_KEYS` and to every
  `CONFIGS` row.  The parser fails if a guard mentions a predicate that the
  feature environment does not define.
- **New LLT spelling**: extend `expr.llt_name()` / `llt_bits()`; the corpus
  format uses those strings, so a rename is a corpus-format change.
- **New boundary panel**: change `target.BOUNDARY_PANEL` or `DEEP_PANEL`.
  These are review constants, intentionally not derived from the source.

## Coverage manifest

Every points/corpus document carries a coverage manifest, and both commands
exit 3 when a rule has no verified probe and no reviewed exception:

```json
"coverage": {
  "rules": {
    "source_total": 277, "active": 273, "verified": 263,
    "probed_candidates": 263,
    "inactive": [{"rule_set": "X86_FNEG", "rule_index": 0,
                  "reason": "feature predicate false"}],
    "excepted": [{"rule_set": "...", "rule_index": 3, "reason": "..."}],
    "unprobed": []
  },
  "opcodes": {
    "total": 115, "multi_opcode_builders": 19, "secondary_opcodes": 53,
    "per_opcode_atoms": {"X86_EXTRACT": {"G_EXTRACT": 17, "G_INSERT": 18}},
    "per_opcode_shape_atoms": {"X86_EXTRACT": {"G_EXTRACT": 17,
                                               "G_INSERT": 17}},
    "uncovered": []
  },
  "policy": "..."
}
```

- `verified` means the candidate shape is the **first match** under the
  modelled chain predicates, not merely that the rule's own predicate is
  true.
- A builder such as `getActionDefinitionsBuilder({G_EXTRACT, G_INSERT})` gets
  rule, boundary, and address-space probes for **every** opcode.  Lambdas that
  branch on `Query.Opcode` are evaluated per opcode, including lambda-local
  variables such as `SubIdx = Query.Opcode == G_EXTRACT ? 0 : 1`.
- `per_opcode_atoms` counts every origin; `uncovered` and
  `per_opcode_shape_atoms` count only `rule`, `boundary`, and `as` atoms, so
  an opcode that only has an `opcode` probe is still reported as a gap.
- `--allow-coverage-gaps` downgrades the exit-3 gate for inspection only; do
  not use it for a committed corpus.

### Registering a coverage exception

1. Run `points`/`corpus`; exit 3 lists every unprobed rule.
2. Confirm why it cannot be probed: unreachable under the preceding clamps,
   shadowed by an earlier match, or an opaque lambda with no literal witness.
3. Add an entry to `coverage_exceptions.json`:

   ```json
   {"rule_set": "X86_ADD", "rule_index": 9, "method": "clampMinNumElements",
    "configs": ["C0", "C3"],
    "reason": "a one-element fixed vector is a scalar; earlier clamps win"}
   ```

   Matching fields are `rule_set` (required), `rule_index`, `method`, `arg`,
   `arg_contains`, and `config`/`configs`.
4. The reason must describe the source semantics, not "the test fails".  An
   exception is reviewed together with the code change that needs it.

## Comparing corpora

`diff` joins two corpora on the canonical query key
`(config fingerprint, opcode, types, memory type, align bits)`.  It never
joins on atom ids, array order, or raw bytes:

```bash
python3 llvm/utils/x86-legalizer-corpus diff \
    --left /tmp/up-corpus.json --right /tmp/ref-corpus.json \
    --out /tmp/diff.json
```

The report contains `expected_diff`, `left_only`, `right_only`,
`per_rule_coverage`, and `unmapped_rule_sets`.  `expected` is compared only
where both sides carry one: a shapes-only corpus against a filled corpus
reports `expected_available: false`, never a false "0 differences".
`--strict` exits 1 on any difference.

Version-1 documents are read as well.  They carry no `opcode`; when the other
side maps a rule set to exactly one opcode, that mapping is used.  A
multi-opcode builder that cannot be resolved is listed in
`unmapped_rule_sets` instead of being guessed.

Cross-revision example: `upstream/main` corpus vs the frozen-reference corpus
has `expected_diff: []` over the 2133 common query keys and two
`right_only` probes, the `G_CTPOP` rules gated by `HasVPOPCNTDQ` and
`HasVPOPCNTDQVL`.  That is the intended signal: the C++ source moved, the
shared behavior did not.

Shapes-only and filled corpora share one atom shape:
`{"id", "origin", "rule_set", "opcode", "rule_index", "config", "query":
{"types": [...], "mem": {"type", "align"}}}`, where pointers carry their bit
width (`p0:64`, `v2p0:64`).  The intermediate points file used by the C++
filler keeps the short spellings and top-level `types`/`mem`/`align`.

### Migrating a v1 artifact

The committed witness is version 1 (`llvm.gisel.x86-legalizer-witness`).  Do
not rewrite it.  Generate a v2 copy next to it when a tool needs the new atom
shape:

```bash
python3 llvm/utils/x86-legalizer-corpus migrate \
    --input llvm/unittests/Target/X86/x86-legalizer-witness.json \
    --out /tmp/x86-witness-v2.json
```

`migrate` takes the rule-set -> primary-opcode map from the document's
`source_commit`; if that revision does not contain the hand-written chain
(the current v1 witness was generated from the TableGen path), it falls back
to the frozen reference and records which source it used.  The migrated
document keeps its per-atom `config` and does not synthesize a feature map;
regenerate points with `corpus` if you need to fill expected answers.

### Nightly drift check

`upstream-nightly.sh` runs the pipeline against `upstream/main`:

```bash
llvm/utils/x86-legalizer-corpus/upstream-nightly.sh /tmp/x86-nightly
```

It fails when the reader cannot parse the source or when `points` reports an
unregistered coverage gap.  The `diff` against the committed witness is
informational: a cross-revision query-domain difference (today the CTPOP
delta) is expected and does not fail the job.

## Deltas and verification

`verify` compares the C++ rule ledger with the TableGen-emitted rule index and
classifies every difference:

| Class | Meaning |
|---|---|
| `match` | same rule count |
| `td_expands_lambda` | TableGen expanded one predicate over several classes |
| `td_only` | TableGen has rules the C++ chain did not record |
| `input_missing` | the C++ chain has more rules than TableGen |
| `no_cpp_builder` | a TableGen row has no builder in the source |

Every non-`match` class must be registered in `known_deltas.json` as a
`(rule_set, class)` pair with a reason.  The default registry is read
automatically; pass `--known-deltas` to point at another file.

Exit codes:

| Code | Meaning |
|---|---|
| 0 | every delta is registered |
| 2 | the source has an unknown chain method (use `--allow-opaque` to inspect) |
| 3 | at least one delta is not registered |

Running `verify` against `upstream/main` is a useful negative test: the main
branch has two fewer `G_CTPOP` rules, so the run exits 3 with
`X86_CTPOP:td_only`.  That is the tool detecting a real input difference, not
a corpus bug.

## Determinism and tests

Same input, same bytes: every collection is sorted by a stable key before JSON
emission, and points are serialized with `sort_keys=True`.

```bash
python3 -m unittest discover -s llvm/utils/x86-legalizer-corpus/tests -v
```

The suite covers the lexer, the expression evaluator, branch/loop expansion,
the frozen-reference census, point determinism, the oracle boundary, the
coverage manifest, corpus comparison, v1 migration, and the delta registry.  Tests that need the frozen revision or a built
`X86LegalizerRuleIndex.inc` skip when those artifacts are absent.

Frozen-revision census (a change here means the reader or the source moved):

```
call_sites=58  builders=62  opcodes=115  classes=24  mem_classes=23  aligns=1
chain_rules=277  tablegen_rows=63
```

## Recording a query trace from a real compile

A generated corpus asks the rules in enumeration order: one rule set's atoms
together, every class pair, every memory descriptor.  A compile does not.  To
measure the decision path in the order a worklist produces, record one:

```bash
llc -global-isel -O0 -debug-only=legalizer in.ll -o /dev/null 2>&1 \
  | python3 llvm/utils/x86-legalizer-corpus/trace-from-log.py \
      --ast /tmp/ast-C2.json --out /tmp/trace.json
```

The trace is query shapes only, with no answers, so it is not a corpus and
`diff` does not apply to it.  It is replayed by the reader benchmark:

```bash
X86_LEGALIZER_BENCH_CORPUS_IN=<corpus.json> \
X86_LEGALIZER_BENCH_TRACE_IN=/tmp/trace.json \
X86Tests --gtest_also_run_disabled_tests \
  --gtest_filter='*BenchmarkChainVersusTable*'
```

Two approximations are recorded in the envelope rather than hidden: a function
argument's type is never printed by the legalizer, and the log does not print
`TypeIdx`, so types are deduplicated by value.
